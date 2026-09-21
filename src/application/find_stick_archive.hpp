// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <string>

#include "application/use_cases/manage_stick_backups.hpp"

namespace seabass::application
{

// This stick's backup in `directory`, whatever the file is called.
//
// The Back Up page builds its archive path from the stick's LABEL. A
// backup that was given a name is renamed to that name, so the page went
// looking for <label>.zip, found nothing, and reported "no backup of this
// stick yet". The obvious next action then writes a SECOND full archive
// under the label while the named one sits beside it, invisible to the
// page that made it. On a 30 GB stick that is half an hour and 30 GB to
// produce a copy of something that was already there.
//
// Matched on the stick IDENTIFIER, because the name is the thing that has
// changed and the label is what a second stick can share. The label is a
// fallback only for a backup that recorded no identifier, which is every
// backup written before identifiers existed -- and then only the first
// such match, since nothing distinguishes two of them.
//
// Returns an empty path when nothing matches, which is the ordinary case
// for a stick that has never been backed up.
//
// Opens every archive in the folder to read its manifest, so callers use
// it off the GUI thread and only when the expected path is absent: an
// archive that IS at the expected path is this stick's by construction.
inline std::filesystem::path findStickArchive(const std::filesystem::path &directory,
                                                const std::string &stickIdentifier,
                                                const std::string &stickLabel)
{
    if (directory.empty()) {
        return {};
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        return {};
    }
    std::filesystem::path byLabel;
    for (const ManagedStickBackup &backup : ManageStickBackups::list(directory, {})) {
        const StickBackupDescription &d = backup.description;
        if (!d.error.empty()) {
            continue;
        }
        if (!stickIdentifier.empty() && d.stickIdentifier == stickIdentifier) {
            return d.archivePath;
        }
        if (d.stickIdentifier.empty() && d.stickLabel == stickLabel && byLabel.empty()) {
            byLabel = d.archivePath;
        }
    }
    return byLabel;
}

}  // namespace seabass::application
