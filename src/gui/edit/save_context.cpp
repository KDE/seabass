#include "gui/edit/save_context.hpp"

#include "infrastructure/backup/stick_write_lock.hpp"

#include "infrastructure/backup/stick_space.hpp"

#include <map>
#include <set>
#include <vector>

#include <filesystem>

#include "application/path_key.hpp"
#include "infrastructure/backup/filesystem_backup_store.hpp"
#include "infrastructure/backup/stick_locks.hpp"
#include "infrastructure/logging/file_operation_log.hpp"

namespace seabass::gui
{

namespace fs = std::filesystem;

SaveContext::SaveContext(application::CancellationToken cancel, application::ProgressReporter &progress,
                         StatusSink status, QString rekordboxPath, QString enginePath)
    : m_cancel(std::move(cancel)),
      m_progress(progress),
      m_status(std::move(status)),
      m_rekordboxPath(std::move(rekordboxPath)),
      m_enginePath(std::move(enginePath))
{
}

// Shared resources die here, after the finish hooks ran (see
// runFinishHooks): a FormatWriteSession's scratch directory is removed
// only once its commit hook has had its chance.
SaveContext::~SaveContext() = default;

void SaveContext::status(const QString &text)
{
    if (m_status) {
        m_status(text);
    }
}

QString SaveContext::pathFor(const QString &format) const
{
    return format == "engine" ? m_enginePath : m_rekordboxPath;
}

std::string SaveContext::stickRoot() const
{
    const QString &any = m_rekordboxPath.isEmpty() ? m_enginePath : m_rekordboxPath;
    return infrastructure::backup::stickRootForCatalogPath(any.toStdString());
}

application::OperationLog &SaveContext::log()
{
    if (!m_log) {
        m_log = std::make_unique<infrastructure::logging::FileOperationLog>(
            infrastructure::backup::operationLogForStickRoot(stickRoot()));
    }
    return *m_log;
}

application::BackupStore &SaveContext::backupStore()
{
    return archiveStore();
}

infrastructure::backup::FilesystemBackupStore &SaveContext::archiveStore()
{
    if (!m_backupStore) {
        m_backupStore = std::make_unique<infrastructure::backup::FilesystemBackupStore>(
            infrastructure::backup::backupDirForStickRoot(stickRoot()));
    }
    return *m_backupStore;
}

std::uint64_t SaveContext::releaseAutomaticBackupsIfTight()
{
    const auto space = infrastructure::backup::measureStickSpace(stickRoot());
    if (space.capacityBytes == 0) {
        // measureStickSpace() returns zeros for a stick it could not read
        // rather than throwing. Nothing measured means nothing deleted:
        // "I could not tell how full it is" is not a reason to start
        // removing the user's undo history.
        return 0;
    }
    const std::uint64_t headroom = space.headroomBytes();
    if (space.freeBytes >= headroom) {
        return 0;
    }

    try {
        // The same lock the Backups page takes for every action, so a
        // record cannot be deleted out from under another session's
        // restore or listing.
        infrastructure::backup::StickWriteLock lock(
            infrastructure::backup::backupDirForStickRoot(stickRoot()) + "/.write.lock");
        return archiveStore().releaseAutomaticBackups(headroom - space.freeBytes);
    } catch (const std::exception &) {
        // Another session holds the lock. Releasing space is an
        // opportunistic tidy-up, never the point of the save, so it is
        // dropped rather than retried or reported.
        return 0;
    }
}

std::vector<std::string> SaveContext::walSidecarsOf(const std::string &file)
{
    std::vector<std::string> sidecars;
    if (fs::path(file).extension() != ".db") {
        return sidecars;
    }
    std::error_code ec;
    const std::string wal = file + "-wal";
    if (fs::is_regular_file(wal, ec) && fs::file_size(wal, ec) > 0 && !ec) {
        sidecars.push_back(wal);
    }
    return sidecars;
}

void SaveContext::backupAllNow(const std::vector<BackupTarget> &targets)
{
    // Grouped by label, in first-seen order, because a save may hold
    // several kinds of change from one page and each keeps its own record.
    //
    // Deduplicated (both here and against m_backedUp) by
    // normalizedPathKey(), not the raw string: two call sites can spell
    // the same file differently (one built with fs::path's native
    // separators, another by plain string concatenation with a literal
    // '/') and did, on Windows -- the raw-string dedup let export.pdb
    // through twice, backed up under two "different" keys for the one
    // real file.
    std::vector<std::string> labelOrder;
    std::map<std::string, std::vector<std::string>> byLabel;
    std::set<std::string> seen;
    auto add = [&](const std::string &file, const std::string &label) {
        if (file.empty() || m_backedUp.contains(application::normalizedPathKey(file))
            || !seen.insert(application::normalizedPathKey(file)).second) {
            return;
        }
        if (!byLabel.contains(label)) {
            labelOrder.push_back(label);
        }
        byLabel[label].push_back(file);
    };
    for (const auto &target : targets) {
        for (const std::string &sidecar : walSidecarsOf(target.file)) {
            add(sidecar, target.label);
        }
        add(target.file, target.label);
    }

    for (const std::string &label : labelOrder) {
        const std::vector<std::string> &files = byLabel[label];
        auto existing = m_recordByLabel.find(label);
        application::BackupRecord record;
        if (existing == m_recordByLabel.end()) {
            record = archiveStore().backup(files, label);
            m_recordByLabel[label] = record.id;
            m_backups.push_back({QString::fromStdString(fs::path(record.path).parent_path().string()),
                                 QString::fromStdString(record.id)});
        } else {
            record = archiveStore().addToArchive(existing->second, files);
        }
        log().record(label + ": backed up " + std::to_string(files.size()) + " file(s) -> " + record.path);
        for (const std::string &file : files) {
            m_backedUp[application::normalizedPathKey(file)] = record.id;
        }
    }
}

bool SaveContext::backupOnce(const std::string &file, const std::string &label)
{
    // See backupAllNow()'s own comment: keyed by normalizedPathKey(), not
    // the raw string, so a file already backed up under one spelling of
    // its path is recognised under another.
    if (file.empty() || m_backedUp.contains(application::normalizedPathKey(file))) {
        return false;
    }
    // A SQLite database in WAL mode keeps committed pages in its -wal
    // until a checkpoint; a backup of the main file alone would restore
    // an older state than the one on the stick. Back the sidecar up
    // first, under the same label, so the record holds the whole set.
    for (const std::string &sidecar : walSidecarsOf(file)) {
        backupOnce(sidecar, label);
    }
    auto existing = m_recordByLabel.find(label);
    application::BackupRecord record;
    // One deflated archive per save rather than a directory of loose
    // copies. A save that removes a cue from 200 tracks used to make 200
    // durable whole-file writes here -- about 118 ms each on Linux -- and
    // leave 71.5 MB on a stick permanently; the same files are 45.5 MB in
    // an archive. The central directory is rewritten and made durable
    // after every file, so the record stays complete and readable at
    // every point a crash could happen, exactly as the loose layout was.
    // See docs/write-path-performance.md, rounds 9-16.
    if (existing == m_recordByLabel.end()) {
        record = archiveStore().backup({file}, label);
        m_recordByLabel[label] = record.id;
        m_backups.push_back({QString::fromStdString(fs::path(record.path).parent_path().string()),
                             QString::fromStdString(record.id)});
    } else {
        record = archiveStore().addToArchive(existing->second, {file});
    }
    log().record(label + ": backed up " + fs::path(file).filename().string() + " -> " + record.path);
    m_backedUp[application::normalizedPathKey(file)] = record.id;
    return true;
}

std::string SaveContext::backupIdOf(const std::string &file) const
{
    auto it = m_backedUp.find(application::normalizedPathKey(file));
    return it == m_backedUp.end() ? std::string() : it->second;
}

void SaveContext::onFinish(std::function<void(bool)> hook)
{
    m_finishHooks.push_back(std::move(hook));
}

std::optional<QString> SaveContext::runFinishHooks(bool ok)
{
    if (m_hooksRan) {
        return std::nullopt;
    }
    m_hooksRan = true;
    std::optional<QString> firstError;
    for (auto &hook : m_finishHooks) {
        try {
            hook(ok);
        } catch (const std::exception &e) {
            if (!firstError) {
                firstError = QString::fromStdString(e.what());
            }
        }
    }
    return firstError;
}

std::vector<UndoableBackup> SaveContext::takeBackups()
{
    return std::move(m_backups);
}

}  // namespace seabass::gui
