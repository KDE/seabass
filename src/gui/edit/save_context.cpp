// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "gui/edit/save_context.hpp"
#include "infrastructure/backup/interrupted_save.hpp"

#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/fs_remove.hpp"
#include "infrastructure/long_paths.hpp"
#include "infrastructure/durable_file_write.hpp"
#include <algorithm>
#include <fstream>
#include <chrono>
#include <array>

#include "infrastructure/stick_backup/sqlite_db_set.hpp"


#include "infrastructure/backup/stick_space.hpp"

#include <map>
#include <optional>
#include <set>
#include <vector>

#include <filesystem>

#include "application/path_key.hpp"
#include "gui/qt_path.hpp"
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

// Before the first catalog file is written, the stick says which records
// this save made, so a save that never finishes can still be undone by a
// session opened later; see infrastructure/backup/interrupted_save.hpp.
// The session removes the note when the save finishes.
void SaveContext::noteSaveInProgress()
{
    std::vector<std::string> ids;
    for (const UndoableBackup &backup : m_backups) {
        ids.push_back(backup.id.toStdString());
    }
    const std::string backupDir = infrastructure::backup::backupDirForStickRoot(stickRoot());
    if (ids.empty()) {
        // Every record this save made is gone again: nothing to undo from,
        // and no note to say otherwise.
        infrastructure::backup::clearSaveInProgress(backupDir);
        return;
    }
    if (!infrastructure::backup::noteSaveInProgress(backupDir, ids)) {
        // The backup itself is there; only the way back after an
        // interruption is not. Said, not a reason to refuse the save.
        log().record("could not note the save in progress in " + backupDir
                     + "; if this save is interrupted, undo it from Manage Backups");
    }
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
    return releaseAutomaticBackupsIfTight(infrastructure::backup::measureStickSpace(pathFromUtf8(stickRoot())));
}

std::uint64_t SaveContext::releaseAutomaticBackupsIfTight(const infrastructure::backup::StickSpace &space)
{
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
        // No lock is taken here. The stick's write lock is already held,
        // by whoever started this save (LibraryEditSession takes it around
        // the whole save loop), and StickWriteLock is flock() on an open
        // file description: deliberately NOT reentrant, so a second
        // acquisition from inside the first one throws StickBusyError
        // exactly as a second session would. Taking it here therefore did
        // not protect the records, it guaranteed this never ran -- the
        // catch below turned every call into a silent "released 0 bytes".
        // The caller's lock is the stronger guarantee and covers the whole
        // save, this tidy-up included.
        //
        // Every record this save made, not only the newest: together they
        // are the undo the user has just been offered.
        std::set<std::string> thisSave;
        for (const UndoableBackup &backup : m_backups) {
            thisSave.insert(backup.id.toStdString());
        }
        return archiveStore().releaseAutomaticBackups(headroom - space.freeBytes, thisSave);
    } catch (const std::exception &e) {
        // Still never fails the save: this is an opportunistic tidy-up on
        // a save that already succeeded, and a warning saying "could not
        // free space" beside "your changes are saved" would read as half
        // a failure. But it no longer passes in silence. The old handler
        // returned 0 and said nothing, which is how a nested-lock throw
        // on every single call went unnoticed for as long as it did: the
        // stick filled up, the number said zero, and zero is also what a
        // stick with plenty of room reports.
        if (hasStick()) {
            log().record(std::string("save: the stick is below its headroom and the automatic backups "
                                     "could not be released: ")
                         + e.what());
        }
        return 0;
    }
}

std::vector<std::string> SaveContext::walSidecarsOf(const std::string &file)
{
    // The same definition of "the database's set" Full Stick Backup
    // uses: the main file plus whichever of -wal / -journal exist.
    std::vector<std::string> sidecars;
    const fs::path filePath = pathFromUtf8(file);
    if (filePath.extension() != ".db") {
        return sidecars;
    }
    for (const fs::path &member : infrastructure::stick_backup::dbSetMembers(filePath)) {
        std::error_code ec;
        if (member != filePath && fs::is_regular_file(member, ec) && fs::file_size(member, ec) > 0 && !ec) {
            sidecars.push_back(pathToUtf8(member));
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

    // The records this call makes, so that when a later label's backup
    // fails (the stick filled up) the earlier ones go with it: the save
    // is refused as a whole, and a record with no save behind it would
    // sit in Manage Backups taking space on a stick that has none. A
    // label that already had a record keeps it -- addToArchive() leaves
    // a record exactly as it was when it fails.
    std::vector<std::string> madeHere;
    const size_t backupsBefore = m_backups.size();
    try {
        for (const std::string &label : labelOrder) {
            const std::vector<std::string> &files = byLabel[label];
            auto existing = m_recordByLabel.find(label);
            application::BackupRecord record;
            if (existing == m_recordByLabel.end()) {
                record = archiveStore().backup(files, label);
                madeHere.push_back(record.id);
                m_recordByLabel[label] = record.id;
                m_backups.push_back({pathToQString(pathFromUtf8(record.path).parent_path()),
                                     QString::fromStdString(record.id)});
                noteSaveInProgress();
            } else {
                record = archiveStore().addToArchive(existing->second, files);
            }
            log().record(label + ": backed up " + std::to_string(files.size()) + " file(s) -> " + record.path);
            for (const std::string &file : files) {
                m_backedUp[application::normalizedPathKey(file)] = record.id;
            }
        }
    } catch (...) {
        int removedHere = 0;
        std::vector<std::string> stayedHere;
        for (const std::string &madeId : madeHere) {
            // Counted, not assumed. remove() answers false without
            // throwing when the record will not go, and a record still
            // on the stick that has been dropped from m_backups is one
            // nothing can undo from and nothing will clean up -- under a
            // log line saying it was removed. discardBackupsTakenThisSave()
            // below was fixed for exactly this ("Counting calls rather
            // than removals is how a leftover survives under a log line
            // saying it was removed"); this path was not.
            if (archiveStore().remove(madeId)) {
                ++removedHere;
                std::erase_if(m_recordByLabel, [&](const auto &entry) { return entry.second == madeId; });
                std::erase_if(m_backedUp, [&](const auto &entry) { return entry.second == madeId; });
            } else {
                stayedHere.push_back(madeId);
            }
        }
        // Only the records that really went are forgotten -- by id, not
        // by count. The first version of this resized the list back only
        // when EVERY removal succeeded, which is the opposite of what
        // this comment said: one record that would not go kept every
        // successfully DELETED record in m_backups, and takeBackups()
        // then handed the save loop an Undo pointing at an archive that
        // is no longer on the stick.
        //
        // A record that stayed keeps its place, so the save still knows
        // it is there; a record that went leaves, whatever happened to
        // the others.
        std::erase_if(m_backups, [&](const UndoableBackup &backup) {
            const std::string id = backup.id.toStdString();
            return std::find(madeHere.begin(), madeHere.end(), id) != madeHere.end()
                && std::find(stayedHere.begin(), stayedHere.end(), id) == stayedHere.end();
        });
        // The note followed m_backups up; it follows it down too, or it
        // would name records that are gone.
        noteSaveInProgress();
        if (!madeHere.empty()) {
            log().record("backup failed: removed " + std::to_string(removedHere) + " of "
                         + std::to_string(madeHere.size()) + " record(s) this save had already made");
        }
        for (const std::string &id : stayedHere) {
            log().record("backup failed: record " + id + " could NOT be removed and is still on the stick");
        }
        throw;
    }
}

void SaveContext::discardBackupsTakenThisSave()
{
    if (m_recordByLabel.empty()) {
        return;
    }
    // Never throws out of here: this runs after a save has already failed
    // and been put back, so the caller is in the middle of reporting that
    // failure. Anything escaping would take the progress bar and the
    // summary with it.
    try {
        // No lock is taken here, for the reason spelled out in
        // releaseAutomaticBackupsIfTight(): the save already holds the
        // stick's write lock and flock() does not nest, so asking for it
        // again threw StickBusyError on every single save. Round 4's
        // leftover record was not a missing call -- this function ran,
        // failed on its own lock, and said so only in the stick log.
        int removed = 0;
        std::vector<std::string> stayed;
        for (const auto &[label, id] : m_recordByLabel) {
            // remove() ANSWERS, it does not throw: FilesystemBackupStore
            // works in error_code overloads throughout. Counting calls
            // rather than removals is how a leftover survives under a log
            // line saying it was removed, which is the shape of the very
            // bug this function exists to fix.
            if (archiveStore().remove(id)) {
                ++removed;
            } else {
                stayed.push_back(id);
            }
        }
        m_recordByLabel.clear();
        m_backedUp.clear();
        m_backups.clear();
        // The note named those records; with them gone it would promise an
        // undo of a save that changed nothing.
        noteSaveInProgress();
        if (hasStick() && removed > 0) {
            log().record("save: nothing was applied and everything went back, so " + std::to_string(removed)
                         + " backup record(s) this save had taken were removed");
        }
        if (hasStick() && !stayed.empty()) {
            std::string list;
            for (const std::string &id : stayed) {
                list += (list.empty() ? "" : ", ") + id;
            }
            log().record("save: " + std::to_string(stayed.size())
                         + " backup record(s) this save had taken could NOT be removed and are still on the stick: "
                         + list);
        }
    } catch (const std::exception &e) {
        m_recordByLabel.clear();
        m_backedUp.clear();
        m_backups.clear();
        if (hasStick()) {
            log().record(std::string("save: the backup records this save took could not be cleared up: ") + e.what());
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
        m_backups.push_back({pathToQString(pathFromUtf8(record.path).parent_path()),
                             QString::fromStdString(record.id)});
        noteSaveInProgress();
    } else {
        record = archiveStore().addToArchive(existing->second, {file});
    }
    log().record(label + ": backed up " + pathToUtf8(pathFromUtf8(file).filename()) + " -> " + record.path);
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
    if (fs::file_size(pathFromUtf8(a), ec) != fs::file_size(pathFromUtf8(b), ec) || ec) {
        return false;
    }
    std::ifstream left(pathFromUtf8(a), std::ios::binary);
    std::ifstream right(pathFromUtf8(b), std::ios::binary);
    // "I could not read them" is not "they are the same". Neither
    // stream was checked, so if either would not open -- a sharing
    // violation on Windows, a path this filesystem will not resolve --
    // the loop never ran and this answered true. Its one caller is
    // rollBackChange(), which then skips the file: nothing put back,
    // nothing in notPutBack, nothing in firstError, and the log records
    // the rollback as complete.
    if (!left.is_open() || !right.is_open()) {
        return false;
    }
    std::array<char, 65536> l{};
    std::array<char, 65536> r{};
    while (left && right) {
        left.read(l.data(), l.size());
        right.read(r.data(), r.size());
        if (left.gcount() != right.gcount() || !std::equal(l.begin(), l.begin() + left.gcount(), r.begin())) {
            return false;
        }
    }
    // And a read that stopped partway is not a match either. `while
    // (left && right)` leaves on badbit as readily as on eofbit, so a
    // stick that refused halfway through the comparison used to fall
    // out of the loop and return true. Both streams have to have
    // reached the end for "identical" to mean anything -- the same
    // lesson as hashFile() and copyFileDurablyAtomic(), which is now
    // three places in this codebase where a truncated read passed for a
    // complete one.
    return left.eof() && right.eof() && !left.bad() && !right.bad();
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
    const std::string target = writeTargetFor(file);
    // A SQLite database is its sidecars too. Absent ones are recorded as
    // absent, so one the change created is removed again.
    std::vector<std::string> members{target};
    if (pathFromUtf8(target).extension() == ".db") {
        members.push_back(target + "-wal");
        members.push_back(target + "-journal");
    }
    for (const std::string &member : members) {
        if (!m_protected.insert(application::normalizedPathKey(member)).second) {
            continue;
        }
        Checkpoint checkpoint{member, {}};
        const fs::path memberPath = pathFromUtf8(member);
        std::error_code ec;
        if (fs::is_regular_file(memberPath, ec)) {
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
            const std::uintmax_t size = fs::file_size(memberPath);
            const std::uintmax_t available = fs::space(m_checkpointDir->path).available;
            if (available < size) {
                throw std::runtime_error("not enough temporary space to protect " + pathToUtf8(memberPath.filename())
                                         + " before writing it: needs " + std::to_string(size / (1024 * 1024))
                                         + " MB in " + pathToUtf8(m_checkpointDir->path.parent_path()) + ", "
                                         + std::to_string(available / (1024 * 1024)) + " MB free");
            }
            // A counter, not m_checkpoints.size(): see the member's own
            // comment. The vector shrinks under redirectWrites() and the
            // names must not be reissued when it does.
            const fs::path copy = m_checkpointDir->path / std::to_string(m_nextCheckpointName++);
            checkpoint.copy = pathToUtf8(copy);
            fs::copy_file(memberPath, copy, fs::copy_options::overwrite_existing);
        }
        m_checkpoints.push_back(std::move(checkpoint));
    }
}

std::string SaveContext::writeTargetFor(const std::string &liveFile) const
{
    if (auto redirect = m_redirects.find(application::normalizedPathKey(liveFile)); redirect != m_redirects.end()) {
        return redirect->second;
    }
    return liveFile;
}

void SaveContext::redirectWrites(const std::string &liveFile, const std::string &writtenFile)
{
    m_redirects[application::normalizedPathKey(liveFile)] = writtenFile;
    // The live file is not written again until the session commits, after
    // every change, so a copy of it taken earlier in this change is dead
    // weight -- a whole database held in temp beside the scratch copy and
    // that copy's own checkpoint.
    std::vector<std::string> live{liveFile};
    if (pathFromUtf8(liveFile).extension() == ".db") {
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
                fs::remove(pathFromUtf8(checkpoint.copy), ec);
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
    // Which registered databases this save actually wrote -- asked now,
    // because the next line destroys the writers that know.
    std::set<std::string> written;
    for (const auto &[key, database] : m_walDatabases) {
        if (database.wrote && database.wrote()) {
            written.insert(key);
        }
    }

    // Writers first. A SQLite connection that closes checkpoints its WAL
    // into the database, so closing one after the restore would write the
    // failed change straight back over it.
    m_shared.clear();

    std::optional<QString> firstError;
    int putBack = 0;
    // Which files did NOT go back. A database with any of its set in here
    // has its .db from one generation beside sidecars from another.
    std::set<std::string> notPutBack;
    for (auto it = m_checkpoints.rbegin(); it != m_checkpoints.rend(); ++it) {
        try {
            std::error_code ec;
            if (it->copy.empty()) {
                // No copy means the file did not exist before this change,
                // so putting it back means deleting it.
                //
                // Through removeEntry() rather than fs::remove(): the
                // plain call answers "did I unlink something" and reports
                // a path it cannot resolve as a quiet false with no error
                // -- past MAX_PATH on Windows, which a track under a long
                // artist/album path reaches. That left a file a failed
                // change had created sitting on the stick while the save
                // reported the rollback complete. Treated like the error
                // below, because a rollback that did not roll back is the
                // same kind of lie either way.
                // The existence test is long-path prefixed too. Measured
                // on Windows against a real file at a 337-unit path:
                // fs::exists() unprefixed returns FALSE with ec unset, so
                // the guard this replaced short-circuited and never even
                // reached the remove. Prefixing only the remove would
                // have left the bug exactly where it was.
                const fs::path original = infrastructure::longPathSafe(pathFromUtf8(it->original));
                // "Is it there" can fail without being a no: EIO on a
                // dying stick, a sharing violation on Windows, a parent
                // directory that cannot be searched. fs::exists() answers
                // all of those with false and sets ec, and taking that
                // false at face value would count the file as already
                // gone -- no put-back, nothing in notPutBack, and a
                // rollback that reports itself complete while a file the
                // failed change created is still on the stick. That is
                // the same lie as a remove that did not remove, so it
                // goes the same way.
                const bool stillThere = fs::exists(original, ec);
                if (ec) {
                    throw std::runtime_error("could not tell whether " + it->original
                                             + " is still there: " + ec.message());
                }
                if (stillThere) {
                    std::string failure;
                    if (!infrastructure::removeEntry(pathFromUtf8(it->original), failure)) {
                        throw std::runtime_error("could not remove " + it->original + ": " + failure);
                    }
                    ++putBack;
                }
                continue;
            }
            // Byte for byte, not by mtime: FAT keeps two-second mtimes, so
            // a same-size rewrite inside that window looks untouched.
            if (fs::exists(pathFromUtf8(it->original), ec) && sameBytes(it->copy, it->original)) {
                continue;
            }
            if (!infrastructure::copyFileDurablyAtomic(it->copy, it->original)) {
                throw std::runtime_error("could not put back " + it->original);
            }
            ++putBack;
        } catch (const std::exception &e) {
            notPutBack.insert(application::normalizedPathKey(it->original));
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
            const std::string base = checkpoint.original.substr(0, checkpoint.original.size() - suffix.size());
            fs::remove(pathFromUtf8(base + "-shm"), ec);
        }
    }

    // Then the logs themselves. Keyed on the databases this save WROTE, not
    // on the files the failing change protected: a save whose OneLibrary
    // rows came from earlier changes leaves a log nobody folds when the
    // change that fails never touched that database. The writers were
    // destroyed before the restore (above), so their close did whatever
    // passive checkpoint it could and nothing will try again.
    for (const auto &[key, database] : m_walDatabases) {
        const bool mixed = notPutBack.contains(key)
                           || notPutBack.contains(application::normalizedPathKey(database.path + "-wal"))
                           || notPutBack.contains(application::normalizedPathKey(database.path + "-journal"));
        if (mixed) {
            // One generation's database beside another's sidecars. Leaving
            // them in place is NOT inert: SQLite replays a valid -wal (or
            // rolls back a -journal) into whatever .db sits beside it on the
            // next open, with no cross-check -- and something opens it within
            // seconds. So the sidecars are renamed out of SQLite's reach.
            // Renamed rather than removed, the way FilesystemBackupStore
            // treats a restore's stale sidecars but keeping the bytes: this
            // path is already a failure, and nothing here should destroy
            // what someone may still want back.
            std::vector<std::string> moved;
            for (const char *suffix : {"-wal", "-shm", "-journal"}) {
                const fs::path side = pathFromUtf8(database.path + suffix);
                std::error_code ec;
                if (!fs::exists(side, ec) || ec) {
                    continue;
                }
                fs::path stale = side;
                stale += ".seabass-stale";
                fs::remove(stale, ec);
                fs::rename(side, stale, ec);
                if (!ec) {
                    moved.push_back(pathToUtf8(side.filename()));
                }
            }
            if (!moved.empty() && hasStick()) {
                std::string list;
                for (const std::string &name : moved) {
                    list += (list.empty() ? "" : ", ") + name;
                }
                log().record("save: moved " + list + " aside as .seabass-stale -- the rollback could not put "
                             + pathToUtf8(pathFromUtf8(database.path).filename())
                             + " back in step with them, and SQLite would replay them into the wrong generation "
                               "on the next open. The backup taken before this save holds the state to go back to.");
            }
            continue;
        }
        if (!written.contains(key)) {
            // The save never opened this database for writing, so there is
            // no log of its own to fold -- and opening it read-write here
            // would touch a log someone else (a player) may have left.
            continue;
        }
        const std::optional<std::uint64_t> left = database.fold(database.path);
        if (!hasStick()) {
            continue;
        }
        if (!left) {
            log().record("save: after the rollback, could not tell whether "
                         + pathToUtf8(pathFromUtf8(database.path).filename())
                         + " still has a write-ahead log beside it");
        } else if (*left > 0) {
            log().record("save: after the rollback " + pathToUtf8(pathFromUtf8(database.path).filename())
                         + " kept " + std::to_string(*left)
                         + " bytes in its write-ahead log; the rows of the changes that did apply are not in it "
                           "until something folds them");
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

void SaveContext::noteWalDatabase(const std::string &dbPath,
                                  std::function<std::optional<std::uint64_t>(const std::string &)> fold,
                                  std::function<bool()> wrote)
{
    m_walDatabases.insert(
        {application::normalizedPathKey(dbPath), WalDatabase{dbPath, std::move(fold), std::move(wrote)}});
}

void SaveContext::onFinish(std::function<void(bool)> hook)
{
    m_finishHooks.push_back(std::move(hook));
}

SaveContext::FinishOutcome SaveContext::runFinishHooks(bool ok)
{
    FinishOutcome outcome;
    if (m_hooksRan) {
        return outcome;
    }
    m_hooksRan = true;
    for (auto &hook : m_finishHooks) {
        try {
            hook(ok);
        } catch (const SaveTidyUpFailed &e) {
            // Same shape as the fold below: what the save wrote is on the
            // stick, and something beside it is not. The hook has already
            // put the detail in the operation log; this is the half the
            // person sees.
            log().record(std::string("save: ") + e.what());
            if (!outcome.warning) {
                outcome.warning = QString::fromUtf8(e.what());
            }
        } catch (const seabass::infrastructure::onelibrary::OneLibraryLogNotFolded &e) {
            // The rows are committed; only the fold is missing. Nothing of
            // ours will fold it later -- finishWriting() has already closed
            // every connection -- so the frames sit outside exportLibrary.db
            // until something else opens and checkpoints it. Hence saying so
            // out loud. What it must never do is report the save as
            // unapplied: that would have the user save again and apply every
            // removal a second time.
            log().record(std::string("save: ") + e.what());
            if (!outcome.warning) {
                outcome.warning = QString::fromStdString(e.what());
            }
        } catch (const std::exception &e) {
            if (!outcome.error) {
                outcome.error = QString::fromStdString(e.what());
            }
        }
    }
    return outcome;
}

std::vector<UndoableBackup> SaveContext::takeBackups()
{
    return std::move(m_backups);
}

}  // namespace seabass::gui
