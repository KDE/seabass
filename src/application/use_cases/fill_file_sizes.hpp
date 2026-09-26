// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

#include "domain/track.hpp"

namespace seabass::application
{

// Sets Track::fileSizeBytes from the audio file on disk, for the callers
// that need a size. The readers used to stat every audio file while they
// read the catalog; on a stick that is a few thousand stats, seconds cold,
// paid by every page that only wanted titles. So the readers stopped, and
// the size became a stage of its own (see LibraryReader).
//
// Only a size nobody knows yet is filled: a row whose fileSizeBytes is 0.
// rekordbox and Engine never record a size, so every one of their rows is
// filled; OneLibrary carries a size in its own catalog, and that one is
// kept, exactly as its reader always returned it.
//
// A file that cannot be stat'd (missing, or a path this platform cannot
// open) leaves 0, the same "unknown" the readers wrote. Each distinct
// filePath is stat'd once, however many rows name it. Keyed by the exact
// string, not a normalized key: on a case sensitive filesystem two
// spellings can be two files, and on a case insensitive one the second
// stat answers the same, so the exact string is right on both.
//
// Checks nothing else and throws nothing: a size is supplementary, and a
// stick pulled halfway leaves the rest at 0, which reads as "unknown".
void fillFileSizes(std::vector<domain::Track> &tracks);

}  // namespace seabass::application
