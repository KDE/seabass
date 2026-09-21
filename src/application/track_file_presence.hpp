// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>

#include "domain/track.hpp"

namespace seabass::application
{

// Whether a catalog row's audio file is actually there to be played.
//
// is_regular_file, NOT exists. A directory exists, and a directory at a
// track's path is a shape real sticks produce -- a half-finished copy, or
// a filesystem repair that turned a cross-linked chain into a directory
// entry. exists() called that healthy, so Library Health told the person
// the file was fine and the failure only appeared later, when something
// tried to read it. Found by the X3 rig check (tools/rig_file_failure.cpp),
// which plants exactly that and asks whether anyone is told.
//
// Lives here rather than inline at each call site because it was inline
// at each call site: LibraryConsistencyController and rig_read had
// separate copies of the same wrong rule, and a fix to one would have
// left the other disagreeing about what "missing" means.
//
// Streaming rows are not this function's business -- they have no local
// file by definition and every caller skips them first.
inline bool trackFileIsPresent(const domain::Track &track)
{
    if (track.filePath.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(track.filePath, ec);
}

}  // namespace seabass::application
