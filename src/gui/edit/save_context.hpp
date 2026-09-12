#pragma once

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

// Everything one Save shares across the changes it applies. Lives on the
// worker thread for exactly one save loop; never touched by the GUI.
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
    // Automatic records only. What the user asked for is the user's, and
    // the newest automatic record is kept whatever happens, because it is
    // the one Undo Last Save needs.
    //
    // Call only after a save that SUCCEEDED. After a failure or a cancel
    // the backups are precisely the thing that saves you.
    std::uint64_t releaseAutomaticBackupsIfTight();

    // Backs `file` up under `label` unless this save already did; records
    // the backup for undo. Returns true when a backup was made now.
    bool backupOnce(const std::string &file, const std::string &label);
    // Backs every target up in one pass, grouped by label, before the save
    // applies anything. One archive per label rather than one append per
    // file, so a 201-cue save pays one durable write instead of 201 -- and
    // the whole backup is on the stick before the first live file is
    // touched, which the per-item path could not promise.
    void backupAllNow(const std::vector<BackupTarget> &targets);
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

    void onFinish(std::function<void(bool ok)> hook);
    // Runs every hook once, creation order; a throwing hook does not stop
    // the rest. Returns the first hook error, if any.
    std::optional<QString> runFinishHooks(bool ok);

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
    std::map<std::string, std::shared_ptr<void>> m_shared;
    std::vector<std::function<void(bool)>> m_finishHooks;
    bool m_hooksRan = false;
};

}  // namespace seabass::gui
