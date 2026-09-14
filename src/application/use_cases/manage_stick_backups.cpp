// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/manage_stick_backups.hpp"

#include <algorithm>
#include <memory>
#include <system_error>

#include "domain/library_fingerprint.hpp"
#include "infrastructure/backup/stick_write_lock.hpp"
#include "infrastructure/stick_backup/archive_journal.hpp"

namespace seabass::application
{

namespace fs = std::filesystem;

namespace
{

bool samePath(const fs::path &a, const fs::path &b)
{
    if (a.empty() || b.empty()) {
        return false;
    }
    std::error_code ec;
    if (fs::equivalent(a, b, ec) && !ec) {
        return true;
    }
    return a.lexically_normal() == b.lexically_normal();
}

}  // namespace

std::vector<ManagedStickBackup> ManageStickBackups::list(const fs::path &directory, const fs::path &currentArchive)
{
    std::vector<ManagedStickBackup> backups;
    if (directory.empty()) {
        return backups;
    }
    for (StickBackupDescription &description : RestoreStickBackup::describeAll(directory)) {
        ManagedStickBackup backup;
        backup.isCurrentStick = samePath(description.archivePath, currentArchive);
        if (description.error.empty()) {
            if (auto fingerprint = domain::LibraryFingerprint::parse(description.libraryFingerprint)) {
                backup.trackCount = fingerprint->trackCount;
                backup.playlistCount = fingerprint->playlistCount;
            }
        }
        backup.description = std::move(description);
        backups.push_back(std::move(backup));
    }
    // describeAll's order (readable newest first, unreadable last) within
    // each half.
    std::stable_partition(backups.begin(), backups.end(),
                          [](const ManagedStickBackup &backup) { return backup.isCurrentStick; });
    return backups;
}

DeleteStickBackupResult ManageStickBackups::remove(const fs::path &archivePath)
{
    DeleteStickBackupResult result;
    std::error_code ec;
    if (!fs::is_regular_file(archivePath, ec)) {
        result.message = "The backup " + archivePath.filename().string() + " is no longer there.";
        return result;
    }

    const fs::path lockPath = infrastructure::stick_backup::journal::lockPathFor(archivePath);
    std::unique_ptr<infrastructure::backup::StickWriteLock> lock;
    try {
        lock = std::make_unique<infrastructure::backup::StickWriteLock>(lockPath.string());
    } catch (const infrastructure::backup::StickBusyError &) {
        result.status = DeleteStickBackupResult::Status::Busy;
        result.message = "Something is writing to " + archivePath.filename().string()
            + " right now (a backup, a restore or a compaction). Delete it once that has finished.";
        return result;
    } catch (const std::exception &e) {
        result.message = std::string("Could not lock the backup to delete it: ") + e.what();
        return result;
    }

    if (!fs::remove(archivePath, ec) || ec) {
        result.message = "Could not delete " + archivePath.filename().string() + ": "
            + (ec ? ec.message() : std::string("it is no longer there"));
        return result;
    }
    // The journal goes too: left behind, the next backup under this name
    // would try to recover an archive that is no longer there.
    std::error_code ignored;
    fs::remove(infrastructure::stick_backup::journal::journalPathFor(archivePath), ignored);
    // The lock file stays. Removing a lock file is how two writers end up
    // holding "the" lock at once -- one on the unlinked file, one on a new
    // one -- and on Windows a held lock file cannot be removed at all. The
    // next backup under this name takes it again.
    result.status = DeleteStickBackupResult::Status::Deleted;
    return result;
}

}  // namespace seabass::application
