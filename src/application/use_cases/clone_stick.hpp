// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "application/use_cases/backup_stick.hpp"
#include "application/use_cases/restore_stick_backup.hpp"

namespace seabass::application
{

// Copying a library from one stick onto another, as a chain of the two
// operations that already exist and are trusted: an incremental full
// stick backup of the source into its own archive on disk, then a
// restore of that archive onto the target. No second copy engine; the
// disk backup that is left behind is the point of a backup stick anyway.
// See docs/stick-backup-plan.md, "Stick-to-stick clone".

struct CloneProgress
{
    enum class Stage
    {
        Backup,   // reading the source into the archive
        Restore,  // writing the archive onto the target
    };
    Stage stage = Stage::Backup;
    BackupProgress backup;    // valid during Stage::Backup
    RestoreProgress restore;  // valid during Stage::Restore
};

struct CloneStickOptions
{
    // The source side: stick root, archive path, identifier, label,
    // fingerprint, conflict probe. Its `cancel` and `onProgress` are
    // replaced by the ones below.
    BackupStickOptions backup;
    std::filesystem::path targetRoot;  // a mounted, formatted volume (or any directory)
    // Overlay (default): files on the target that are not in the source
    // stay. Exact: they are removed after the copy.
    bool exact = false;
    CancellationToken cancel = CancellationToken::none();
    std::function<void(const CloneProgress &)> onProgress;
    RestoredLibraryCheck libraryCheck;
    std::uint64_t freeSpaceMarginBytes = 64u << 20;
};

struct CloneStickPreview
{
    std::string error;  // non-empty: the clone cannot run (target is the source, not a directory, ...)
    BackupPreview backup;         // what the backup step would do
    bool archiveCurrent = false;  // the backup step would be a no-op
    // What the restore step would do -- only known when the archive
    // already exists (an update); a first clone has nothing to plan from.
    std::optional<RestorePreview> restore;
    std::uint64_t sourceBytes = 0;  // every file on the source stick
    std::uint64_t bytesToTarget = 0;  // best estimate of what the restore step writes
    std::uint64_t targetFreeBytes = 0;
    bool enoughTargetSpace = true;
    bool targetHasEngineLibrary = false;
};

struct CloneStickOutcome
{
    enum class Status
    {
        Cloned,
        ClonedWithProblems,  // rejected entries, missing tracks or write errors -- see `restore`
        Refused,             // never started: target is the source
        BackupIncomplete,    // the backup step did not capture the library (DJ software appeared, database too large or unstable, failure); the target was not touched
        RestoreFailed,       // the backup step succeeded, the restore step could not write the target
        Cancelled,           // during either step; a partial backup is kept so the next run resumes
    };
    Status status = Status::RestoreFailed;
    std::string message;
    BackupOutcomeStatus backupStatus = BackupOutcomeStatus::Failed;
    std::uint64_t backupBytesRead = 0;
    bool restoreStarted = false;
    RestoreSummary restore;  // valid when restoreStarted
};

class CloneStick
{
public:
    static CloneStickPreview preview(const CloneStickOptions &options);
    static CloneStickOutcome execute(const CloneStickOptions &options,
                                     ProgressReporter &reporter = NullProgressReporter::instance());
};

std::string_view toString(CloneStickOutcome::Status status);

}  // namespace seabass::application
