// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/track.hpp"

namespace seabass::application
{

// The size of one file on disk, or nothing when it is not there or its
// path cannot be opened on this platform (on Windows, pathFromUtf8 throws
// for bytes that are not valid UTF-8, which a raw catalog column can
// hold). The one place a track's file is sized: fillFileSizes() and
// measureRealFileSizes() both ask here, so a fix to how a path reaches
// the filesystem reaches both.
std::optional<std::uint64_t> fileSizeOnDisk(const std::string &utf8Path);

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
// Checks nothing else: a size is supplementary, and a stick pulled
// halfway leaves the rest at 0, which reads as "unknown". The one thing it
// throws is OperationCancelled: `cancel` is checked before every file, so
// a stick pulled mid-pass (the catalog cache cancels its prefetch) costs
// at most the one stat already under way, not the rest of the library's.
// `progress` ticks once per file stat'd, with the count so far.
void fillFileSizes(std::vector<domain::Track> &tracks,
                   CancellationToken cancel = CancellationToken::none(),
                   ProgressReporter &progress = NullProgressReporter::instance());

// Clears artworkPath on every track whose image is not on disk: one stat
// per distinct path. The Engine and OneLibrary readers used to do this
// inside every read; it is a stage of its own now, with the sizes, so a
// page that only wants titles does not pay ~1500 stats on a stick.
// `cancel` and `progress` as for fillFileSizes(): checked before, and
// ticked after, every image looked for.
void dropMissingArtwork(std::vector<domain::Track> &tracks,
                        CancellationToken cancel = CancellationToken::none(),
                        ProgressReporter &progress = NullProgressReporter::instance());

// What a read of a whole library owes its caller now that the readers
// read the catalog alone: every size (fillFileSizes()), and the covers
// that are not on disk dropped (dropMissingArtwork()) for the rows of
// the catalogs whose readers used to check them, Engine and OneLibrary.
// A rekordbox row keeps the artwork its catalog names, exactly as its
// reader always returned it (Track::format says which catalog a row came
// from). The catalog cache's Full stage is this, and so is
// ScanLibrary::execute() and every direct readAll() caller in the CLI
// and the tools, so a page and the command line agree on a catalog.
// `cancel` and `progress` as for the two it runs.
void completeTracks(std::vector<domain::Track> &tracks,
                    CancellationToken cancel = CancellationToken::none(),
                    ProgressReporter &progress = NullProgressReporter::instance());

}  // namespace seabass::application
