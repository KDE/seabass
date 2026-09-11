// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/clone_stick.hpp"

#include <memory>
#include <system_error>
#include <utility>

#include "infrastructure/engine/engine_library_layout.hpp"

namespace seabass::application
{

namespace fs = std::filesystem;

namespace
{

bool sameLocation(const fs::path &a, const fs::path &b)
{
    std::error_code ec;
    if (fs::equivalent(a, b, ec)) {
        return true;
    }
    return a.lexically_normal() == b.lexically_normal();
}

std::uint64_t availableBytes(const fs::path &directory)
{
    std::error_code ec;
    const fs::space_info space = fs::space(directory, ec);
    return ec ? 0 : space.available;
}

BackupStickOptions backupOptionsFor(const CloneStickOptions &options)
{
    BackupStickOptions backup = options.backup;
    backup.cancel = options.cancel;
    backup.onProgress = {};
    if (options.onProgress) {
        backup.onProgress = [&options](const BackupProgress &progress) {
            CloneProgress clone;
            clone.stage = CloneProgress::Stage::Backup;
            clone.backup = progress;
            options.onProgress(clone);
        };
    }
    return backup;
}

RestoreOptions restoreOptionsFor(const CloneStickOptions &options)
{
    RestoreOptions restore;
    restore.archivePath = options.backup.archivePath;
    restore.targetRoot = options.targetRoot;
    restore.exact = options.exact;
    restore.cancel = options.cancel;
    restore.libraryCheck = options.libraryCheck;
    restore.freeSpaceMarginBytes = options.freeSpaceMarginBytes;
    if (options.onProgress) {
        restore.onProgress = [&options](const RestoreProgress &progress) {
            CloneProgress clone;
            clone.stage = CloneProgress::Stage::Restore;
            clone.restore = progress;
            options.onProgress(clone);
        };
    }
    return restore;
}

}  // namespace

CloneStickPreview CloneStick::preview(const CloneStickOptions &options)
{
    CloneStickPreview preview;
    std::error_code ec;
    if (sameLocation(options.backup.stickRoot, options.targetRoot)) {
        preview.error = "The source and the target are the same drive.";
        return preview;
    }
    if (!fs::is_directory(options.targetRoot, ec)) {
        preview.error = "The target is not a mounted drive.";
        return preview;
    }

    preview.backup = BackupStick::preview(backupOptionsFor(options));
    if (!preview.backup.error.empty()) {
        preview.error = preview.backup.error;
        return preview;
    }
    preview.sourceBytes = preview.backup.stickBytes;
    preview.archiveCurrent = preview.backup.archiveExists && preview.backup.added == 0 && preview.backup.changed == 0
                             && preview.backup.removed == 0 && !preview.backup.databaseChanged;

    // With an archive on disk the restore step can be planned exactly;
    // whatever the backup step adds to it will be written too.
    preview.bytesToTarget = preview.sourceBytes;
    if (preview.backup.archiveExists) {
        RestorePreview restore = RestoreStickBackup::preview(restoreOptionsFor(options));
        if (restore.error.empty()) {
            preview.bytesToTarget = restore.bytesToWrite + preview.backup.bytesToRead;
            preview.restore = std::move(restore);
        }
    }
    preview.targetHasEngineLibrary = fs::exists(infrastructure::engine::engineMainDatabasePath(options.targetRoot), ec);
    preview.targetFreeBytes = availableBytes(options.targetRoot);
    preview.enoughTargetSpace = preview.targetFreeBytes >= preview.bytesToTarget + options.freeSpaceMarginBytes;
    return preview;
}

CloneStickOutcome CloneStick::execute(const CloneStickOptions &options, ProgressReporter &reporter)
{
    CloneStickOutcome outcome;
    if (sameLocation(options.backup.stickRoot, options.targetRoot)) {
        outcome.status = CloneStickOutcome::Status::Refused;
        outcome.message = "The source and the target are the same drive.";
        return outcome;
    }

    BackupStickOutcome backup = BackupStick::execute(backupOptionsFor(options), reporter);
    if (backup.status == BackupOutcomeStatus::Cancelled && backup.pending) {
        // Keep, never discard: the next run resumes from here instead of
        // re-reading the whole stick.
        std::unique_ptr<PendingBackup> pending = std::move(backup.pending);
        backup = pending->keep();
        backup.status = BackupOutcomeStatus::Cancelled;
    }
    outcome.backupStatus = backup.status;
    outcome.backupBytesRead = backup.bytesRead;
    switch (backup.status) {
    case BackupOutcomeStatus::Complete:
    case BackupOutcomeStatus::NothingToDo:
        break;
    case BackupOutcomeStatus::Cancelled:
    case BackupOutcomeStatus::KeptPartial:
        outcome.status = CloneStickOutcome::Status::Cancelled;
        outcome.message = "Stopped during the backup step; the partial backup was kept, so running again resumes it. "
                          "The target was not touched.";
        return outcome;
    case BackupOutcomeStatus::Discarded:
    case BackupOutcomeStatus::ConflictAborted:
    case BackupOutcomeStatus::DbTooLarge:
    case BackupOutcomeStatus::DbUnstable:
    case BackupOutcomeStatus::Failed:
        outcome.status = CloneStickOutcome::Status::BackupIncomplete;
        outcome.message = "The backup step did not capture the library: " + backup.message + " The target was not touched.";
        return outcome;
    }

    outcome.restoreStarted = true;
    outcome.restore = RestoreStickBackup::execute(restoreOptionsFor(options), reporter);
    outcome.message = outcome.restore.message;
    switch (outcome.restore.status) {
    case RestoreSummary::Status::Restored: outcome.status = CloneStickOutcome::Status::Cloned; break;
    case RestoreSummary::Status::RestoredWithProblems: outcome.status = CloneStickOutcome::Status::ClonedWithProblems; break;
    case RestoreSummary::Status::Cancelled: outcome.status = CloneStickOutcome::Status::Cancelled; break;
    case RestoreSummary::Status::Failed: outcome.status = CloneStickOutcome::Status::RestoreFailed; break;
    }
    return outcome;
}

std::string_view toString(CloneStickOutcome::Status status)
{
    switch (status) {
    case CloneStickOutcome::Status::Cloned: return "cloned";
    case CloneStickOutcome::Status::ClonedWithProblems: return "cloned-with-problems";
    case CloneStickOutcome::Status::Refused: return "refused";
    case CloneStickOutcome::Status::BackupIncomplete: return "backup-incomplete";
    case CloneStickOutcome::Status::RestoreFailed: return "restore-failed";
    case CloneStickOutcome::Status::Cancelled: return "cancelled";
    }
    return "restore-failed";
}

}  // namespace seabass::application
