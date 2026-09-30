// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/recordings_controller.hpp"

#include <QDateTime>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <filesystem>
#include <set>

#include "gui/detached_write.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/future_result.hpp"
#include "gui/main_thread_shared.hpp"
#include "gui/qt_path.hpp"
#include "gui/sleep_inhibitor.hpp"
#include "gui/stick_path.hpp"
#include "infrastructure/audio/duration_fill.hpp"
#include "infrastructure/backup/stick_locks.hpp"
#include "infrastructure/backup/stick_write_lock.hpp"
#include "infrastructure/logging/file_operation_log.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;
using application::RecordingOutcome;

// ---- RecordingListModel --------------------------------------------

int RecordingListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant RecordingListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }
    const auto &row = m_rows[static_cast<std::size_t>(index.row())];
    switch (role) {
        case PathRole:
            return qtPathFromUtf8(row.path);
        case FileNameRole:
            return QString::fromUtf8(row.fileName);
        case FolderNameRole:
            return QString::fromUtf8(row.folderName);
        case SourceRole:
            return QString::fromLatin1(application::recordingSourceKey(row.source));
        case SizeBytesRole:
            return static_cast<qlonglong>(row.sizeBytes);
        case ModifiedRole:
            return row.modifiedUnix > 0 ? QDateTime::fromSecsSinceEpoch(row.modifiedUnix) : QDateTime();
        case DurationSecondsRole:
            return row.durationSeconds ? *row.durationSeconds : -1.0;
        case IncludedRole:
            return static_cast<bool>(m_included[static_cast<std::size_t>(index.row())]);
        default:
            return {};
    }
}

bool RecordingListModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role != IncludedRole || !index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return false;
    }
    m_included[static_cast<std::size_t>(index.row())] = value.toBool();
    emit dataChanged(index, index, {IncludedRole});
    return true;
}

QHash<int, QByteArray> RecordingListModel::roleNames() const
{
    return {
        {PathRole, "path"},
        {FileNameRole, "fileName"},
        {FolderNameRole, "folderName"},
        {SourceRole, "source"},
        {SizeBytesRole, "sizeBytes"},
        {ModifiedRole, "modified"},
        {DurationSecondsRole, "durationSeconds"},
        {IncludedRole, "included"},
    };
}

void RecordingListModel::setRecordings(std::vector<application::Recording> recordings)
{
    beginResetModel();
    m_rows = std::move(recordings);
    m_included.assign(m_rows.size(), false);
    endResetModel();
}

void RecordingListModel::setAllIncluded(bool included)
{
    if (m_rows.empty()) {
        return;
    }
    m_included.assign(m_rows.size(), included);
    emit dataChanged(index(0), index(rowCount() - 1), {IncludedRole});
}

std::vector<std::string> RecordingListModel::includedPaths() const
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (m_included[i]) {
            out.push_back(m_rows[i].path);
        }
    }
    return out;
}

int RecordingListModel::includedCount() const
{
    return static_cast<int>(std::count(m_included.begin(), m_included.end(), true));
}

std::uint64_t RecordingListModel::includedBytes() const
{
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (m_included[i]) {
            total += m_rows[i].sizeBytes;
        }
    }
    return total;
}

// ---- RecordingsController ------------------------------------------

namespace
{

QString stickRootFor(const QString &rekordboxPath, const QString &enginePath)
{
    const QString catalog = !enginePath.isEmpty() ? enginePath : rekordboxPath;
    return catalog.isEmpty() ? QString() : stickRootOf(catalog);
}

application::RecordingDeleteHooks &deleteHooks()
{
    static application::RecordingDeleteHooks hooks;
    return hooks;
}

RecordingsRunResult runTask(QString stickRootQt, std::vector<std::string> paths,
                            std::shared_ptr<RecordingsProgressRelay> relay, application::CancellationToken cancel,
                            application::RecordingDeleteHooks hooks)
{
    RecordingsRunResult result;
    try {
        const fs::path stickRoot = pathFromQString(stickRootQt);
        // The lock every other writer on this stick takes (stick_locks.hpp):
        // a sync or a save from another Seabass instance must contend with
        // this, even though none of them touches these folders, because
        // Seabass assumes one writer on a stick at a time.
        infrastructure::backup::StickWriteLock lock(pathFromUtf8(infrastructure::backup::writeLockPathForBackupDir(
            infrastructure::backup::backupDirForStickRoot(pathToUtf8(stickRoot)))));
        infrastructure::logging::FileOperationLog log(pathToUtf8(infrastructure::paths::stickOperationLog(stickRoot)));

        // No full stick backup, no pending-deletions entry and no copy, on
        // purpose: see the class comment in recordings_controller.hpp.
        result.report = application::deleteRecordings(
            stickRoot, paths, cancel,
            [&relay](const application::RecordingsProgress &p) {
                emit relay->progressed(p.fileIndex, p.fileCount, QString::fromUtf8(p.fileName));
            },
            hooks);

        using Status = RecordingOutcome::Status;
        for (const auto &o : result.report.outcomes) {
            switch (o.status) {
                case Status::Deleted:
                    log.record("recordings: permanently deleted " + o.path + " (no copy kept, at the user's request)");
                    break;
                case Status::DeleteFailed:
                case Status::Refused:
                case Status::Vanished:
                    log.record("recordings: " + o.path + " not deleted: " + o.reason);
                    break;
                case Status::NotAttempted:
                    break;
            }
        }
    } catch (const std::exception &e) {
        result.error = QString::fromUtf8(e.what());
    }
    return result;
}

}  // namespace

void RecordingsController::setDeleteHooksForTesting(application::RecordingDeleteHooks hooks)
{
    deleteHooks() = std::move(hooks);
}

RecordingsController::RecordingsController(QObject *parent) : QObject(parent)
{
    connect(&m_listWatcher, &QFutureWatcher<RecordingsListResult>::finished, this, &RecordingsController::onListed);
    connect(&m_runWatcher, &QFutureWatcher<RecordingsRunResult>::finished, this,
            &RecordingsController::onRunFinished);
}

RecordingsController::~RecordingsController()
{
    // A listing only reads, and briefly: wait for it. A run is a write:
    // asked to stop after the file in flight and watched from the
    // application, which gives the lock back when it returns (the same
    // shape as Delete Orphaned Files; see detached_write.hpp).
    awaitQuietly(m_listWatcher);
    if (m_working) {
        m_cancel.cancel();
        finishWriteDetached(m_runWatcher.future(), m_writeHold.handOver());
    }
}

QString RecordingsController::stickRoot() const
{
    return stickRootFor(m_rekordboxPath, m_enginePath);
}

QVariantMap RecordingsController::summarize(const QString &rekordboxPath, const QString &enginePath) const
{
    const QString root = stickRootFor(rekordboxPath, enginePath);
    if (root.isEmpty()) {
        return {{"count", 0}, {"bytes", 0.0}, {"sources", QStringList()}, {"unreadable", false}};
    }
    application::NullTrackDurationProbe noDurations;
    const auto listing = application::listRecordings(pathFromQString(root), noDurations);
    std::set<std::string> sources;
    for (const auto &r : listing.recordings) {
        sources.insert(application::recordingSourceKey(r.source));
    }
    // In a fixed order, so the card's wording does not depend on names.
    QStringList ordered;
    for (const char *key : {"engine", "pioneer", "alphatheta"}) {
        if (sources.count(key) > 0) {
            ordered << QString::fromLatin1(key);
        }
    }
    return {
        {"count", static_cast<int>(listing.recordings.size())},
        {"bytes", static_cast<double>(listing.totalBytes)},
        {"sources", ordered},
        {"unreadable", !listing.unreadableFolders.empty()},
    };
}

void RecordingsController::load(const QString &stickLabel, const QString &rekordboxPath, const QString &enginePath)
{
    if (busy()) {
        return;
    }
    m_stickLabel = stickLabel;
    m_rekordboxPath = rekordboxPath;
    m_enginePath = enginePath;
    const QString root = stickRoot();
    if (root.isEmpty()) {
        setError(QStringLiteral("This stick has no library folder to find its root from."));
        return;
    }
    m_listing = true;
    emit busyChanged();
    m_listWatcher.setFuture(QtConcurrent::run([root]() {
        RecordingsListResult result;
        try {
            const fs::path stick = pathFromQString(root);
            // TagLib reads a WAV's header only: measured at well under a
            // millisecond for a 2.2 GB file, so every row gets a length.
            result.listing = infrastructure::audio::withDurationProbe(
                [&stick](application::TrackDurationProbe &probe) { return application::listRecordings(stick, probe); });
            // As CleanupController's stickSpace(): 0/0 when it cannot tell.
            std::error_code ec;
            const fs::space_info space = fs::space(stick, ec);
            if (!ec && space.capacity != 0 && space.capacity != static_cast<std::uintmax_t>(-1)) {
                result.stickTotalBytes = static_cast<qlonglong>(space.capacity);
                result.stickFreeBytes = static_cast<qlonglong>(space.available);
            }
        } catch (const std::exception &e) {
            result.error = QString::fromUtf8(e.what());
        }
        return result;
    }));
}

void RecordingsController::onListed()
{
    QString thrown;
    RecordingsListResult result = takeResult(m_listWatcher, &thrown);
    if (!thrown.isEmpty()) {
        result.error = thrown;
    }
    m_listing = false;
    m_listed = result.error.isEmpty();
    m_totalBytes = result.listing.totalBytes;
    m_stickTotalBytes = result.stickTotalBytes;
    m_stickFreeBytes = result.stickFreeBytes;
    m_leftAlone.clear();
    for (const auto &l : result.listing.leftAlone) {
        m_leftAlone.append(QVariantMap{
            {"fileName", QString::fromUtf8(l.fileName)},
            {"folderName", QString::fromUtf8(l.folderName)},
            {"reason", QString::fromUtf8(l.reason)},
        });
    }
    m_unreadableFolders.clear();
    for (const auto &f : result.listing.unreadableFolders) {
        m_unreadableFolders << QString::fromUtf8(f);
    }
    m_model.setRecordings(std::move(result.listing.recordings));
    setError(result.error);
    emit busyChanged();
    emit listingChanged();
    emit selectionChanged();
}

void RecordingsController::setIncluded(int row, bool included)
{
    if (m_model.setData(m_model.index(row), included, RecordingListModel::IncludedRole)) {
        emit selectionChanged();
    }
}

void RecordingsController::setAllIncluded(bool included)
{
    m_model.setAllIncluded(included);
    emit selectionChanged();
}

QStringList RecordingsController::selectedPaths() const
{
    QStringList out;
    for (const std::string &path : m_model.includedPaths()) {
        out << qtPathFromUtf8(path);
    }
    return out;
}

void RecordingsController::deleteSelected()
{
    if (busy() || !m_listed) {
        return;
    }
    std::vector<std::string> paths = m_model.includedPaths();
    if (paths.empty()) {
        return;
    }
    // A write on the stick, so the same lock the library's other writers
    // take: another instance editing this stick's library holds it off.
    const QString libraryId =
        EditSessionRegistry::instance()->libraryIdForPath(!m_enginePath.isEmpty() ? m_enginePath : m_rekordboxPath);
    if (auto refusal = m_writeHold.acquire({libraryId}, m_stickLabel)) {
        if (refusal->showsLockedDialog()) {
            emit lockRefused(refusal->holder);
        }
        return;
    }

    setError({});
    m_cancel = application::CancellationToken();
    m_filesDone = 0;
    m_filesTotal = static_cast<int>(paths.size());
    m_currentItem.clear();
    m_working = true;
    emit progressChanged();
    emit busyChanged();

    auto relay = makeMainThreadShared<RecordingsProgressRelay>();
    connect(relay.get(), &RecordingsProgressRelay::progressed, this,
            [this](int fileIndex, int fileCount, const QString &fileName) {
                m_filesDone = fileIndex;
                m_filesTotal = fileCount;
                m_currentItem = QStringLiteral("Deleting %1").arg(fileName);
                emit progressChanged();
            });

    auto keepAwake = SleepInhibitor::hold(QStringLiteral("Deleting recordings from a USB stick"));
    m_runWatcher.setFuture(QtConcurrent::run(
        [keepAwake, root = stickRoot(), paths = std::move(paths), relay, cancel = m_cancel,
         hooks = deleteHooks()]() mutable {
            return runTask(root, std::move(paths), relay, cancel, std::move(hooks));
        }));
}

void RecordingsController::cancel()
{
    if (!m_working || m_cancel.cancelled()) {
        return;
    }
    m_cancel.cancel();
    emit busyChanged();  // cancelRequested flipped
}

void RecordingsController::onRunFinished()
{
    QString thrown;
    RecordingsRunResult result = takeResult(m_runWatcher, &thrown);
    if (!thrown.isEmpty()) {
        result.error = thrown;
    }
    m_writeHold.release();
    m_working = false;
    emit busyChanged();

    const auto &report = result.report;
    using Status = RecordingOutcome::Status;
    QStringList failures;
    for (const auto &o : report.outcomes) {
        if (o.status == Status::DeleteFailed || o.status == Status::Refused || o.status == Status::Vanished) {
            failures << QStringLiteral("%1 (%2)").arg(QString::fromUtf8(o.fileName), QString::fromUtf8(o.reason));
        }
    }
    QString warning;
    if (!failures.isEmpty()) {
        warning = QStringLiteral("%1 not deleted: %2.")
                      .arg(failures.size() == 1 ? QStringLiteral("One recording was")
                                                : QStringLiteral("%1 recordings were").arg(failures.size()),
                           failures.join(QStringLiteral("; ")));
    }
    emit finished(QVariantMap{
        {"written", report.deleted},
        {"total", report.requested},
        {"unit", QStringLiteral("recordings")},
        {"verb", QStringLiteral("deleted")},
        {"cancelled", report.cancelled},
        {"error", result.error},
        {"warning", warning},
    });
    // What is on the stick now, read again rather than inferred.
    load(m_stickLabel, m_rekordboxPath, m_enginePath);
}

void RecordingsController::setError(const QString &message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = message;
    emit messagesChanged();
}

}  // namespace seabass::gui
