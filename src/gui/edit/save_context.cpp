// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/save_context.hpp"
#include "infrastructure/durable_file_write.hpp"
#include <algorithm>
#include <fstream>
#include <chrono>
#include <array>

#include "infrastructure/stick_backup/sqlite_db_set.hpp"

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
        // Every record this save made, not only the newest: together they
        // are the undo the user has just been offered.
        std::set<std::string> thisSave;
        for (const UndoableBackup &backup : m_backups) {
            thisSave.insert(backup.id.toStdString());
        }
        return archiveStore().releaseAutomaticBackups(headroom - space.freeBytes, thisSave);
    } catch (const std::exception &) {
        // Another session holds the lock. Releasing space is an
        // opportunistic tidy-up, never the point of the save, so it is
        // dropped rather than retried or reported.
        return 0;
    }
}

std::vector<std::string> SaveContext::walSidecarsOf(const std::string &file)
{
    // The same definition of "the database's set" Full Stick Backup
    // uses: the main file plus whichever of -wal / -journal exist.
    std::vector<std::string> sidecars;
    if (fs::path(file).extension() != ".db") {
        return sidecars;
    }
    for (const fs::path &member : infrastructure::stick_backup::dbSetMembers(fs::path(file))) {
        std::error_code ec;
        if (member != fs::path(file) && fs::is_regular_file(member, ec) && fs::file_size(member, ec) > 0 && !ec) {
            sidecars.push_back(member.string());
        }
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
    // Before the dedup: a file backed up once for the whole save is still
    // written again by every later change, and each of those needs its own
    // copy to roll back to.
    protectForThisChange(file);
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

namespace
{

bool sameBytes(const std::string &a, const std::string &b)
{
    std::error_code ec;
    if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) {
        return false;
    }
    std::ifstream left(a, std::ios::binary);
    std::ifstream right(b, std::ios::binary);
    std::array<char, 65536> l{};
    std::array<char, 65536> r{};
    while (left && right) {
        left.read(l.data(), l.size());
        right.read(r.data(), r.size());
        if (left.gcount() != right.gcount() || !std::equal(l.begin(), l.begin() + left.gcount(), r.begin())) {
            return false;
        }
    }
    return true;
}

}  // namespace

void SaveContext::beginChange(const std::vector<BackupTarget> &declared)
{
    m_inChange = true;
    for (const BackupTarget &target : declared) {
        protectForThisChange(target.file);
    }
}

void SaveContext::protectForThisChange(const std::string &file)
{
    if (!m_inChange || file.empty()) {
        return;
    }
    std::string target = file;
    if (auto redirect = m_redirects.find(application::normalizedPathKey(file)); redirect != m_redirects.end()) {
        target = redirect->second;
    }
    // A SQLite database is its sidecars too. Absent ones are recorded as
    // absent, so one the change created is removed again.
    std::vector<std::string> members{target};
    if (fs::path(target).extension() == ".db") {
        members.push_back(target + "-wal");
        members.push_back(target + "-journal");
    }
    for (const std::string &member : members) {
        if (!m_protected.insert(application::normalizedPathKey(member)).second) {
            continue;
        }
        Checkpoint checkpoint{member, {}};
        std::error_code ec;
        if (fs::is_regular_file(member, ec)) {
            if (!m_checkpointDir) {
                fs::path dir = fs::temp_directory_path()
                    / ("seabass-change-checkpoint-"
                       + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
                fs::create_directories(dir);
                m_checkpointDir.emplace(dir);
            }
            // Refused up front with the numbers, rather than as a bare "no
            // space left" halfway through a copy. Either way the change
            // fails before it has written anything.
            const std::uintmax_t size = fs::file_size(member);
            const std::uintmax_t available = fs::space(m_checkpointDir->path).available;
            if (available < size) {
                throw std::runtime_error("not enough temporary space to protect " + fs::path(member).filename().string()
                                         + " before writing it: needs " + std::to_string(size / (1024 * 1024))
                                         + " MB in " + m_checkpointDir->path.parent_path().string() + ", "
                                         + std::to_string(available / (1024 * 1024)) + " MB free");
            }
            checkpoint.copy = (m_checkpointDir->path / std::to_string(m_checkpoints.size())).string();
            fs::copy_file(member, checkpoint.copy, fs::copy_options::overwrite_existing);
        }
        m_checkpoints.push_back(std::move(checkpoint));
    }
}

void SaveContext::redirectWrites(const std::string &liveFile, const std::string &writtenFile)
{
    m_redirects[application::normalizedPathKey(liveFile)] = writtenFile;
    // The live file is not written again until the session commits, after
    // every change, so a copy of it taken earlier in this change is dead
    // weight -- a whole database held in temp beside the scratch copy and
    // that copy's own checkpoint.
    std::vector<std::string> live{liveFile};
    if (fs::path(liveFile).extension() == ".db") {
        live.push_back(liveFile + "-wal");
        live.push_back(liveFile + "-journal");
    }
    for (const std::string &member : live) {
        const std::string key = application::normalizedPathKey(member);
        if (m_protected.erase(key) == 0) {
            continue;
        }
        std::erase_if(m_checkpoints, [&](const Checkpoint &checkpoint) {
            if (application::normalizedPathKey(checkpoint.original) != key) {
                return false;
            }
            std::error_code ec;
            if (!checkpoint.copy.empty()) {
                fs::remove(checkpoint.copy, ec);
            }
            return true;
        });
    }
}

void SaveContext::endChange()
{
    for (auto &hook : m_changeEndHooks) {
        hook(true);
    }
    m_checkpoints.clear();
    m_protected.clear();
    m_checkpointDir.reset();
    m_inChange = false;
}

std::optional<QString> SaveContext::rollBackChange()
{
    // Writers first. A SQLite connection that closes checkpoints its WAL
    // into the database, so closing one after the restore would write the
    // failed change straight back over it.
    m_shared.clear();

    std::optional<QString> firstError;
    int putBack = 0;
    for (auto it = m_checkpoints.rbegin(); it != m_checkpoints.rend(); ++it) {
        try {
            std::error_code ec;
            if (it->copy.empty()) {
                if (fs::exists(it->original, ec) && fs::remove(it->original, ec)) {
                    ++putBack;
                }
                if (ec) {
                    throw std::runtime_error("could not remove " + it->original + ": " + ec.message());
                }
                continue;
            }
            // Byte for byte, not by mtime: FAT keeps two-second mtimes, so
            // a same-size rewrite inside that window looks untouched.
            if (fs::exists(it->original, ec) && sameBytes(it->copy, it->original)) {
                continue;
            }
            if (!infrastructure::copyFileDurablyAtomic(it->copy, it->original)) {
                throw std::runtime_error("could not put back " + it->original);
            }
            ++putBack;
        } catch (const std::exception &e) {
            if (!firstError) {
                firstError = QString::fromStdString(e.what());
            }
        }
    }
    // The -shm indexes a -wal that was just replaced; SQLite rebuilds it.
    for (const Checkpoint &checkpoint : m_checkpoints) {
        const std::string suffix = "-wal";
        if (checkpoint.original.size() > suffix.size()
            && checkpoint.original.compare(checkpoint.original.size() - suffix.size(), suffix.size(), suffix) == 0) {
            std::error_code ec;
            fs::remove(checkpoint.original.substr(0, checkpoint.original.size() - suffix.size()) + "-shm", ec);
        }
    }
    if (hasStick()) {
        log().record(firstError ? "save: putting back what the failed change had written FAILED: "
                                      + firstError->toStdString()
                                : "save: put back " + std::to_string(putBack)
                                      + " file(s) the failed change had written");
    }

    for (auto &hook : m_changeEndHooks) {
        hook(false);
    }
    m_checkpoints.clear();
    m_protected.clear();
    m_checkpointDir.reset();
    m_inChange = false;
    return firstError;
}

void SaveContext::onChangeEnd(std::function<void(bool)> hook)
{
    m_changeEndHooks.push_back(std::move(hook));
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
