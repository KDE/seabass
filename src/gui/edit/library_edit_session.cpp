#include "infrastructure/local/browsed_backup_root.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/future_result.hpp"

#include <QtConcurrent>

#include <filesystem>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <set>

#include "gui/edit/edit_session_registry.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/qt_progress_reporter.hpp"
#include "gui/write_guard.hpp"
#include "infrastructure/backup/filesystem_backup_store.hpp"
#include "infrastructure/backup/stick_locks.hpp"

namespace seabass::gui
{

namespace
{

// The undo of one save: puts every file that save backed up back the way
// it was, newest backup first. Unit "files" because that is what the
// summary can honestly count.
class RestoreBackupsChange : public PendingChange
{
public:
    explicit RestoreBackupsChange(std::vector<UndoableBackup> backups) : m_backups(std::move(backups)) {}

    QString id() const override { return QStringLiteral("undo:last-save"); }
    QString description() const override
    {
        return QStringLiteral("Undo the last save (%1 backup(s))").arg(m_backups.size());
    }
    QString unit() const override { return QStringLiteral("undo steps"); }
    QStringList formatsTouched() const override { return {"rekordbox", "engine", "onelibrary"}; }

    ChangeOutcome apply(SaveContext &ctx) override
    {
        int restored = 0;
        for (auto it = m_backups.rbegin(); it != m_backups.rend(); ++it) {
            infrastructure::backup::FilesystemBackupStore store(it->backupDir.toStdString());
            if (store.restore(it->id.toStdString())) {
                restored++;
            }
        }
        ctx.log().record("undo: restored " + std::to_string(restored) + " backup(s) of the last save");
        return ChangeOutcome::success();
    }

private:
    std::vector<UndoableBackup> m_backups;
};

// Bridges the worker's progress and status lines to the session on the
// GUI thread (same lifetime rule as QtProgressReporter: owned by the
// task via shared_ptr, connected with the session as context object).
class SaveProgressBridge : public QtProgressReporter
{
public:
    using QtProgressReporter::QtProgressReporter;
};

}  // namespace

LibraryEditSession::LibraryEditSession(EditSessionRegistry *registry, QString libraryId, QString stickLabel,
                                       QString mountPoint, QObject *parent)
    : QObject(parent),
      m_registry(registry),
      m_libraryId(std::move(libraryId)),
      m_stickLabel(std::move(stickLabel)),
      m_mountPoint(std::move(mountPoint))
{
    connect(&m_watcher, &QFutureWatcher<SaveLoopResult>::finished, this, &LibraryEditSession::onSaveFinished);
    connect(&m_stickSpaceWatcher, &QFutureWatcherBase::finished, this, [this] {
        // takeResult(), not result(): an exception from the pool thread
        // rethrown in a slot reaches nobody and terminates the process.
        // gui/future_result.hpp exists because that already happened once
        // here, from a real crash dump. A measurement that threw reports
        // zeros, which every reader of these numbers already treats as
        // "not measured" and stays silent about.
        m_stickSpace = gui::takeResult(m_stickSpaceWatcher);
        emit stickSpaceChanged();
    });
}

LibraryEditSession::~LibraryEditSession()
{
    // A save still running keeps its own copies of everything; the
    // process-exit thread-pool wait in main() lets it finish its item.
    if (m_lockHeld && !m_writing) {
        releaseLock();
    }
}

QString LibraryEditSession::state() const
{
    if (m_writing) {
        return QStringLiteral("writing");
    }
    return m_changes.empty() ? QStringLiteral("idle") : QStringLiteral("editing");
}

QStringList LibraryEditSession::pendingDescriptions() const
{
    QStringList lines;
    for (const auto &change : m_changes) {
        lines << change->description();
    }
    return lines;
}

void LibraryEditSession::setStickLabel(const QString &label)
{
    if (m_stickLabel == label) {
        return;
    }
    m_stickLabel = label;
    emit stickLabelChanged();
}

void LibraryEditSession::setLibraryPaths(const QString &rekordboxPath, const QString &enginePath)
{
    if (!rekordboxPath.isEmpty()) {
        m_rekordboxPath = rekordboxPath;
    }
    if (!enginePath.isEmpty()) {
        m_enginePath = enginePath;
    }

    // Measured once here rather than per save: this is where the stick
    // root becomes known, and the answer is shown when an edit page opens
    // so the user can back out before staging anything.
    //
    // On a worker thread, because the measurement walks the whole
    // analysis tree -- one directory per track, so thousands of stats on
    // removable media -- and this runs inside the page's construction,
    // from EditSessionHost's Component.onCompleted. QML completes
    // children before the root, so it happened before the page's own
    // busy overlay existed: clicking Browse Library froze the window for
    // seconds with no spinner and nothing to say why. The answer still
    // arrives well before anything can be staged (the library is
    // read-only until a cue is), which is all the question needs.
    //
    // Nothing here decides where a backup is actually written --
    // save_context.cpp measures that for itself, unremembered, at save
    // time. This number only asks the user a question.
    const QString &any = m_rekordboxPath.isEmpty() ? m_enginePath : m_rekordboxPath;
    if (!any.isEmpty()) {
        const std::filesystem::path root =
            infrastructure::backup::stickRootForCatalogPath(any.toStdString());
        m_stickSpaceWatcher.setFuture(QtConcurrent::run(
            infrastructure::backup::measureStickSpace, root));
    }
}

bool LibraryEditSession::acquireLock()
{
    if (m_lockHeld) {
        return true;
    }
    application::LibraryEditLock lock = m_registry->lockTemplate(m_libraryId, m_stickLabel, m_mountPoint);
    if (m_registry->lockStore().tryAcquire(lock)) {
        m_lockHeld = true;
        emit stateChanged();
        m_registry->sessionStateChanged();
        return true;
    }
    auto probe = m_registry->lockStore().probe(m_libraryId.toStdString(), m_registry->instanceId());
    emit lockRefused(EditSessionRegistry::holderToVariant(probe.holder));
    return false;
}

void LibraryEditSession::releaseLock()
{
    if (!m_lockHeld) {
        return;
    }
    m_registry->lockStore().release(m_libraryId.toStdString(), m_registry->instanceId());
    m_lockHeld = false;
    emit stateChanged();
    m_registry->sessionStateChanged();
}

bool LibraryEditSession::releaseLockIfUnneeded()
{
    if (m_lockHeld && m_refs == 0 && m_changes.empty() && !m_writing) {
        releaseLock();
        return true;
    }
    return false;
}

void LibraryEditSession::heartbeat()
{
    if (m_lockHeld) {
        m_registry->lockStore().heartbeat(m_libraryId.toStdString(), m_registry->instanceId());
    }
}

void LibraryEditSession::release()
{
    if (m_refs > 0) {
        --m_refs;
    }
    releaseLockIfUnneeded();
}

// The batch is owned by one page only while it has changes in it; once it
// is empty any page may start a new batch.
void LibraryEditSession::clearOwnerIfClean()
{
    if (m_changes.empty()) {
        m_editorOwner.clear();
    }
}

bool LibraryEditSession::editsBrowsedBackup() const
{
    for (const QString &libraryPath : {m_rekordboxPath, m_enginePath}) {
        if (libraryPath.isEmpty()) {
            continue;
        }
        if (infrastructure::local::isBrowsedBackupRoot(
                infrastructure::backup::stickRootForCatalogPath(libraryPath.toStdString()))) {
            return true;
        }
    }
    return false;
}

bool LibraryEditSession::stage(std::unique_ptr<PendingChange> change)
{
    if (!change || m_writing) {
        return false;
    }
    // Before anything else, including the lock: a browsed backup is
    // read-only by construction (see MediaController::openBackup), and a
    // refused attempt must leave no trace.
    if (editsBrowsedBackup()) {
        m_registry->reportReadOnlyRefusal(m_libraryId, m_stickLabel);
        return false;
    }
    // Checked before the lock is taken, so a refused attempt leaves no
    // trace: one library is edited by one page at a time, full stop.
    const QString owner = change->owner();
    if (!m_changes.empty() && !m_editorOwner.isEmpty() && owner != m_editorOwner) {
        emit editorConflict(m_editorOwner, owner);
        return false;
    }
    if (!acquireLock()) {
        return false;
    }
    m_editorOwner = owner;
    const QString id = change->id();
    bool wasDirty = dirty();
    auto existing = std::find_if(m_changes.begin(), m_changes.end(),
                                 [&](const auto &c) { return c->id() == id; });
    if (existing != m_changes.end()) {
        *existing = std::shared_ptr<PendingChange>(std::move(change));
    } else {
        m_changes.push_back(std::shared_ptr<PendingChange>(std::move(change)));
    }
    emit pendingChanged();
    if (!wasDirty) {
        emit stateChanged();
        m_registry->sessionStateChanged();
    }
    return true;
}

void LibraryEditSession::unstage(const QString &changeId)
{
    if (m_writing) {
        return;
    }
    auto it = std::remove_if(m_changes.begin(), m_changes.end(), [&](const auto &c) { return c->id() == changeId; });
    if (it == m_changes.end()) {
        return;
    }
    m_changes.erase(it, m_changes.end());
    clearOwnerIfClean();
    emit pendingChanged();
    if (m_changes.empty()) {
        emit stateChanged();
        m_registry->sessionStateChanged();
        releaseLockIfUnneeded();
    }
}

bool LibraryEditSession::hasChange(const QString &changeId) const
{
    return std::any_of(m_changes.begin(), m_changes.end(), [&](const auto &c) { return c->id() == changeId; });
}

void LibraryEditSession::save()
{
    if (m_writing || m_changes.empty()) {
        return;
    }
    if (m_rekordboxPath.isEmpty() && m_enginePath.isEmpty()) {
        m_lastSummary = {{"written", 0}, {"total", pendingCount()}, {"unit", m_changes.front()->unit()},
                         {"cancelled", false}, {"error", QStringLiteral("no library path is known for this session")}};
        emit saveFinished(m_lastSummary);
        return;
    }

    m_savingUnit = m_changes.front()->unit();
    m_savingVerb = m_changes.front()->verb();
    m_writeCancel = application::CancellationToken();
    m_cancelRequested = false;
    setWriteProgress(QStringLiteral("Preparing"), 0, pendingCount());
    setWriting(true);

    auto bridge = std::make_shared<SaveProgressBridge>();
    connect(bridge.get(), &QtProgressReporter::started, this,
            [this](const QString &, int total) { setWriteProgress(m_writeLabel, 0, total); });
    connect(bridge.get(), &QtProgressReporter::progressed, this,
            [this](int current) { setWriteProgress(m_writeLabel, current, m_writeTotal); });
    connect(bridge.get(), &QtProgressReporter::warningRaised, this,
            [this](const QString &text) { setWriteProgress(text, m_writeCurrent, m_writeTotal); });

    std::vector<std::shared_ptr<PendingChange>> changes = m_changes;
    QString rekordboxPath = m_rekordboxPath;
    QString enginePath = m_enginePath;
    application::CancellationToken cancel = m_writeCancel;

    m_watcher.setFuture(QtConcurrent::run([changes, rekordboxPath, enginePath, cancel, bridge]() {
        SaveLoopResult result;
        QString refusal = refuseIfDjSoftwareRunning();
        if (!refusal.isEmpty()) {
            result.error = refusal;
            return result;
        }
        try {
            const QString &any = rekordboxPath.isEmpty() ? enginePath : rekordboxPath;
            auto locks = infrastructure::backup::acquireStickLocks(
                {infrastructure::backup::backupDirForCatalogPath(any.toStdString())});
            auto status = [bridge](const QString &text) { bridge->warn(text.toStdString()); };
            SaveContext ctx(cancel, *bridge, status, rekordboxPath, enginePath);
            result = runSaveLoop(changes, ctx);
        } catch (const std::exception &e) {
            result.error = QString::fromStdString(e.what());
        }
        return result;
    }));
}

void LibraryEditSession::onSaveFinished()
{
    SaveLoopResult result = m_watcher.result();

    std::set<QString> applied(result.appliedIds.begin(), result.appliedIds.end());
    std::set<QString> formats;
    for (const auto &change : m_changes) {
        if (applied.count(change->id())) {
            for (const QString &format : change->formatsTouched()) {
                formats.insert(format);
            }
        }
    }
    m_changes.erase(std::remove_if(m_changes.begin(), m_changes.end(),
                                   [&](const auto &c) { return applied.count(c->id()) > 0; }),
                    m_changes.end());
    clearOwnerIfClean();

    for (const QString &format : formats) {
        const QString path = format == "engine" ? m_enginePath : m_rekordboxPath;
        if (!path.isEmpty()) {
            LibraryCatalogCache::instance().invalidateWithOneLibraryMirror(format.toStdString(), path.toStdString());
        }
    }

    bool undoRan = applied.count(QStringLiteral("undo:last-save")) > 0;
    m_lastBackups = undoRan ? std::vector<UndoableBackup>() : std::move(result.backups);
    emit canUndoChanged();

    m_lastSummary = {
        {"written", static_cast<int>(applied.size())},
        {"total", static_cast<int>(applied.size()) + pendingCount()},
        {"unit", m_savingUnit},
        {"verb", m_savingVerb},
        {"cancelled", result.cancelled},
        {"error", result.error},
        {"failedId", result.failedId},
    };

    setWriting(false);
    for (const QString &id : result.appliedIds) {
        emit changeApplied(id);
    }
    emit pendingChanged();
    emit stateChanged();
    m_registry->sessionStateChanged();
    emit saveFinished(m_lastSummary);
    releaseLockIfUnneeded();
}

void LibraryEditSession::discard()
{
    if (m_writing) {
        return;
    }
    bool hadChanges = !m_changes.empty();
    m_changes.clear();
    clearOwnerIfClean();
    if (hadChanges) {
        emit pendingChanged();
        emit stateChanged();
        m_registry->sessionStateChanged();
        emit changesDiscarded();
    }
    releaseLockIfUnneeded();
}

void LibraryEditSession::cancelWrite()
{
    if (!m_writing || m_cancelRequested) {
        return;
    }
    m_cancelRequested = true;
    m_writeCancel.cancel();
    setWriteProgress(QStringLiteral("Stopping after the current item"), m_writeCurrent, m_writeTotal);
}

void LibraryEditSession::undoLastSave()
{
    if (m_writing || m_lastBackups.empty() || dirty()) {
        return;
    }
    if (stage(std::make_unique<RestoreBackupsChange>(m_lastBackups))) {
        save();
    }
}

void LibraryEditSession::setStickPresent(bool present, const QString &identityStrength)
{
    if (m_stickPresent == present && m_stickIdentityStrength == identityStrength) {
        return;
    }
    m_stickPresent = present;
    m_stickIdentityStrength = identityStrength;
    emit stickPresenceChanged();
}

void LibraryEditSession::setWriting(bool writing)
{
    if (m_writing == writing) {
        return;
    }
    m_writing = writing;
    emit stateChanged();
    m_registry->sessionStateChanged();
}

void LibraryEditSession::setWriteProgress(const QString &label, int current, int total)
{
    m_writeLabel = label;
    m_writeCurrent = current;
    m_writeTotal = total;
    emit writeProgressChanged();
}

}  // namespace seabass::gui
