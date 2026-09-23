// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <set>

#include "infrastructure/scratch_dir_guard.hpp"

#include <cstdint>

#include <QString>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "application/ports/backup_store.hpp"
#include "infrastructure/backup/stick_space.hpp"
#include "application/ports/cancellation_token.hpp"
#include "application/ports/operation_log.hpp"
#include "application/ports/progress_reporter.hpp"
#include "gui/edit/pending_change.hpp"
#include "gui/undo_tracking.hpp"

namespace seabass::infrastructure::backup
{
class FilesystemBackupStore;
}

namespace seabass::gui
{

// Thrown by a finish hook when everything the save wrote landed and only
// a tidy-up beside it did not: reported to the user, never as a failed
// save (see FinishOutcome::warning, and OneLibraryLogNotFolded, which
// runFinishHooks() treats the same way). Telling someone a save failed
// when it did not has them do it again, which for a removal means doing
// it twice.
class SaveTidyUpFailed : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Everything one Save shares across the changes it applies. Lives on the
// worker thread for exactly one save loop; never touched by the GUI.
//
// PRECONDITION: the caller holds the stick's write lock for the whole
// lifetime of this object (LibraryEditSession::save() takes it around the
// save loop). Nothing in here takes that lock itself, and nothing in here
// may: StickWriteLock is flock() on an open file description and does not
// nest, so a second acquisition from inside the save throws StickBusyError
// exactly as a competing session would.
//
// - backupOnce(): the pre-write backup of a file, made at most once per
//   save however many changes touch it. All files backed up under one
//   label go into ONE backup record per save (a cue removed from 200
//   tracks is one entry in Manage Backups, and one restore on undo),
//   which is what the undo list remembers.
// - shared<T>(): per-save, per-key resources -- a format's writer plus
//   its scratch copy (FormatWriteSession), a OneLibrary mirror writer --
//   created by the first change that asks and reused by the rest, so
//   per-item changes keep the batch efficiency the old all-in-one tasks
//   had.
// - onFinish(): hooks run after the loop in creation order, even after a
//   cancel or a failure (a scratch copy commits what completed).
// - beginChange() / endChange() / rollBackChange(): a change lands whole or
//   not at all. Every file it declares, and every file it protects on the
//   way (backupOnce() does, so does a scratch copy), is copied aside before
//   it runs; if it fails, the save's writers are closed and those files are
//   put back, so no catalog keeps half of a change that failed.
class SaveContext
{
public:
    using StatusSink = std::function<void(const QString &)>;

    SaveContext(application::CancellationToken cancel, application::ProgressReporter &progress, StatusSink status,
                QString rekordboxPath, QString enginePath);
    ~SaveContext();
    SaveContext(const SaveContext &) = delete;
    SaveContext &operator=(const SaveContext &) = delete;

    const application::CancellationToken &cancel() const { return m_cancel; }
    application::ProgressReporter &progress() { return m_progress; }
    // "What is going on right now", shown next to the progress bar.
    void status(const QString &text);

    const QString &rekordboxPath() const { return m_rekordboxPath; }
    const QString &enginePath() const { return m_enginePath; }
    // rekordbox and OneLibrary share the PIONEER root.
    QString pathFor(const QString &format) const;
    std::string stickRoot() const;

    application::OperationLog &log();
    application::BackupStore &backupStore();
    // The same store, as itself: the archive-backed record is a
    // FilesystemBackupStore feature, not part of the port.
    infrastructure::backup::FilesystemBackupStore &archiveStore();

    // Deletes automatic backups, oldest first, if this stick has dropped
    // below its headroom -- and only then. Returns the bytes freed.
    //
    // Space is the binding constraint rather than time: a cue backup stays
    // on the stick permanently, nothing prunes it, and both real sticks
    // sampled were 95% and 97% full. While there is room a backup is worth
    // far more than the space it occupies, so this does nothing at all
    // until the stick is genuinely tight.
    //
    // Automatic records only. What the user asked for is the user's. And
    // every record this save made is kept, because together they are what
    // Undo Last Save restores -- a save makes one per kind of change, so
    // keeping only the newest automatic record could cut its own undo in
    // half.
    //
    // Call only after a save that SUCCEEDED. After a failure or a cancel
    // the backups are precisely the thing that saves you.
    std::uint64_t releaseAutomaticBackupsIfTight();
    // The same decision against a StickSpace handed in rather than
    // measured, so a test can put the stick under its headroom without a
    // genuinely full disk. Split out because the version that measures
    // could only ever be tested by filling a real volume, and so was
    // never tested at all -- it spent its whole life throwing
    // StickBusyError on a lock its own caller held and returning 0.
    std::uint64_t releaseAutomaticBackupsIfTight(const infrastructure::backup::StickSpace &space);

    // Backs `file` up under `label` unless this save already did; records
    // the backup for undo. Returns true when a backup was made now.
    bool backupOnce(const std::string &file, const std::string &label);
    // Backs every target up in one pass, grouped by label, before the save
    // applies anything. One archive per label rather than one append per
    // file, so a 201-cue save pays one durable write instead of 201 -- and
    // the whole backup is on the stick before the first live file is
    // touched, which the per-item path could not promise.
    void backupAllNow(const std::vector<BackupTarget> &targets);
    // Throws away the records this save took, for the one case where
    // they are certainly not wanted: nothing was applied and the
    // rollback put everything back, so they are backups of a stick that
    // never changed. Round 5 found one on a stick too full for the save
    // to proceed -- a complete record, backup.zip and manifest, taking
    // space that the save had just refused for want of. NOT called when
    // a change applied (its backup is the way back from it) or when the
    // rollback left anything behind (then it is the only way back).
    void discardBackupsTakenThisSave();
    // The id of the backup this save made for `file` (empty if none yet),
    // for records that want to name it (the pending-deletion manifest).
    std::string backupIdOf(const std::string &file) const;

    template <class T>
    T &shared(const std::string &key, const std::function<std::unique_ptr<T>()> &make)
    {
        auto it = m_shared.find(key);
        if (it == m_shared.end()) {
            std::shared_ptr<T> made(make());
            it = m_shared.emplace(key, std::shared_ptr<void>(made)).first;
        }
        return *static_cast<T *>(it->second.get());
    }

    // The shared() resource for this key if it is still here, nullptr if
    // it never existed or a rolled-back change closed it. For a finish
    // hook, which runs after rollBackChange() may have cleared them all
    // and must not hold a reference across that.
    template <typename T>
    T *sharedIfPresent(const std::string &key)
    {
        auto it = m_shared.find(key);
        return it == m_shared.end() ? nullptr : static_cast<T *>(it->second.get());
    }

    // shared(), for a resource that has to outlive a rolled-back change: a
    // FormatWriteSession, whose commit still has to carry the changes that
    // landed before the one that failed. rollBackChange() closes every
    // shared() resource and none of these.
    template <class T>
    T &sharedForWholeSave(const std::string &key, const std::function<std::unique_ptr<T>()> &make)
    {
        auto it = m_wholeSaveShared.find(key);
        if (it == m_wholeSaveShared.end()) {
            std::shared_ptr<T> made(make());
            it = m_wholeSaveShared.emplace(key, std::shared_ptr<void>(made)).first;
        }
        return *static_cast<T *>(it->second.get());
    }

    // The sharedForWholeSave() resource for this key if a change created
    // one, nullptr if none did.
    template <class T>
    T *sharedForWholeSaveIfPresent(const std::string &key)
    {
        auto it = m_wholeSaveShared.find(key);
        return it == m_wholeSaveShared.end() ? nullptr : static_cast<T *>(it->second.get());
    }

    // One change at a time, driven by runSaveLoop(). beginChange() copies
    // the declared files aside; protectForThisChange() does the same for a
    // file the change is about to write that it did not declare, once per
    // change, and does nothing outside one. Throws when a file cannot be
    // copied, which fails the change before it has written anything.
    void beginChange(const std::vector<BackupTarget> &declared);
    void protectForThisChange(const std::string &file);
    // Writes to `liveFile` land in `writtenFile` for the rest of this save
    // (a scratch copy), so that is the file a failed change must put back.
    void redirectWrites(const std::string &liveFile, const std::string &writtenFile);
    // Where a write to `liveFile` has to go: `liveFile` itself, or the
    // scratch copy a redirect points at. A change that opens a file by
    // path of its own -- rather than through a writer this class hands out
    // -- has to ask, or it writes the live file behind the redirect and
    // the scratch lands on top of its work at the end of the save.
    std::string writeTargetFor(const std::string &liveFile) const;
    void endChange();
    // Closes every shared() writer, then puts back every file this change
    // protected. Returns the first file that could not be put back.
    std::optional<QString> rollBackChange();
    // hook(true) once a change has landed, hook(false) once it was rolled
    // back.
    void onChangeEnd(std::function<void(bool landed)> hook);

    // A database this save wrote through a connection that keeps a
    // write-ahead log, and how to fold it. Registered by whoever opens the
    // writer, so a rolled-back save can fold what its restore brought back
    // without this class knowing any format's filenames -- and so the fold
    // follows the WRITER's database rather than whatever the change that
    // happened to fail had protected.
    // `wrote` is asked before the writers are destroyed on a rollback, so
    // the fold never opens a database the save only looked at.
    void noteWalDatabase(const std::string &dbPath,
                         std::function<std::optional<std::uint64_t>(const std::string &)> fold,
                         std::function<bool()> wrote);

    void onFinish(std::function<void(bool ok)> hook);
    // Runs every hook once, creation order; a throwing hook does not stop
    // the rest.
    struct FinishOutcome
    {
        // A hook failed and what it was committing did not land: the caller
        // must report every change as still pending.
        std::optional<QString> error;
        // The changes DID land and only a tidy-up failed (a write-ahead log
        // that would not fold): worth telling the user, never worth making
        // them save again, which would apply the same removals twice.
        std::optional<QString> warning;
    };
    FinishOutcome runFinishHooks(bool ok);

    std::vector<UndoableBackup> takeBackups();

private:
    // The -wal / -journal beside a SQLite database, when they exist and
    // hold bytes: backed up with the main file so a restore cannot go
    // back to an older state than the stick actually had.
    static std::vector<std::string> walSidecarsOf(const std::string &file);

    application::CancellationToken m_cancel;
    application::ProgressReporter &m_progress;
    StatusSink m_status;
    QString m_rekordboxPath;
    QString m_enginePath;
    std::unique_ptr<application::OperationLog> m_log;
    std::unique_ptr<infrastructure::backup::FilesystemBackupStore> m_backupStore;
    std::map<std::string, std::string> m_backedUp;  // normalizedPathKey(file) -> backup id
    std::map<std::string, std::string> m_recordByLabel;  // label -> this save's record for it
    std::vector<UndoableBackup> m_backups;
    // Declared before m_shared so it is destroyed after it: writers close
    // before the sessions whose scratch copies they may hold open.
    std::map<std::string, std::shared_ptr<void>> m_wholeSaveShared;
    std::map<std::string, std::shared_ptr<void>> m_shared;
    struct WalDatabase
    {
        std::string path;
        std::function<std::optional<std::uint64_t>(const std::string &)> fold;
        std::function<bool()> wrote;
    };
    std::map<std::string, WalDatabase> m_walDatabases;  // normalizedPathKey -> how to fold it
    std::vector<std::function<void(bool)>> m_finishHooks;
    bool m_hooksRan = false;

    // Whether this save has a stick to log to; tests run without one.
    bool hasStick() const { return !m_rekordboxPath.isEmpty() || !m_enginePath.isEmpty(); }

    struct Checkpoint
    {
        std::string original;
        std::string copy;  // empty when the file did not exist
    };
    bool m_inChange = false;
    std::optional<infrastructure::ScratchDirGuard> m_checkpointDir;
    std::vector<Checkpoint> m_checkpoints;
    // Names the checkpoint copies, and NEVER goes down. m_checkpoints
    // used to name them by its own size(), and redirectWrites() erases
    // from that vector -- so the next protect reused a name still in use
    // and copy_file(overwrite_existing) wrote over another checkpoint's
    // saved bytes. A rollback then restored one file's contents onto a
    // different file and reported success.
    std::size_t m_nextCheckpointName = 0;
    std::set<std::string> m_protected;               // normalizedPathKey, this change
    std::map<std::string, std::string> m_redirects;  // normalizedPathKey(live) -> written
    std::vector<std::function<void(bool)>> m_changeEndHooks;
};

}  // namespace seabass::gui
