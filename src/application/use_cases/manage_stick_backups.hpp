// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "application/use_cases/restore_stick_backup.hpp"

namespace seabass::application
{

// One full stick backup as Manage Backups lists it.
struct ManagedStickBackup
{
    StickBackupDescription description;
    // From the library fingerprint the backup recorded; nullopt for a
    // backup written before fingerprints existed, or of an unreadable
    // library.
    std::optional<std::size_t> trackCount;
    std::optional<std::size_t> playlistCount;
    // The backup the stick being looked at relates to (see
    // StickBackupAdvice::backupPath), so the page can put it first.
    bool isCurrentStick = false;
};

struct DeleteStickBackupResult
{
    enum class Status
    {
        Deleted,
        Busy,    // a backup, restore or compaction is writing this archive right now
        Failed,  // not there any more, or the filesystem refused
    };
    Status status = Status::Failed;
    std::string message;
};

// Listing and deleting the full stick backups in the backup folder --
// everything Manage Backups does that is not browsing.
class ManageStickBackups
{
public:
    // Every archive in `directory`: the one at `currentArchive` (may be
    // empty) first, then readable ones newest first, unreadable ones last.
    static std::vector<ManagedStickBackup> list(const std::filesystem::path &directory,
                                                const std::filesystem::path &currentArchive);

    // Deletes the archive together with its journal and lock file. Takes
    // the archive's own write lock first -- the one BackupStick,
    // RestoreStickBackup and CompactStickBackup hold for their whole run --
    // so an archive something is still writing is refused, not pulled out
    // from under it, whichever process or window that is.
    static DeleteStickBackupResult remove(const std::filesystem::path &archivePath);
};

}  // namespace seabass::application
