// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>

namespace seabass::infrastructure
{

// Whether a file can be opened for reading, and if not, whether the
// system said no. A file that exists can still be refused: macOS lets an
// app list a USB stick but not open a file on it until the user has
// allowed it access to removable volumes, and a stick list that only
// asks whether export.pdb exists offers to browse a library nobody can
// read ("could not open .../export.pdb").
enum class ReadAccess {
    Readable,
    Missing,
    // EACCES or EPERM (ERROR_ACCESS_DENIED on Windows): permissions, or
    // the operating system's privacy settings.
    Denied,
    // Anything else: a sharing violation, an I/O error.
    Unreadable,
};

// Opens the file for reading and closes it again; reads nothing.
ReadAccess probeReadAccess(const std::filesystem::path &file);

}  // namespace seabass::infrastructure
