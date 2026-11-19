// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// One audio file on the stick that may be the file a missing track's
// rows point at (#103). The confidence is a tier, never a number: the
// same name and the same size is the file, renamed or moved by hand; the
// same name with another size, or another name with the same tags and
// length, is a file a person has to look at first.
struct MovedFileCandidate
{
    enum class Confidence {
        High,
        NeedsConfirming,
    };

    // The file as the walk found it: filePath, filename, fileSizeBytes,
    // and what its tags said when they could be read (title, artist,
    // durationSeconds), see application::unreferencedFilesAsTracks.
    Track file;
    Confidence confidence = Confidence::NeedsConfirming;
    // Why it is a candidate, in the page's own words.
    std::string reason;
};

// A path the catalogs name and no file answers to, with every row of
// every catalog that names it and the files on the stick that may be
// where it went. One finding per path, not per row: a moved file breaks
// the same track in rekordbox, OneLibrary and Engine alike, and moving it
// back repairs all three at once.
struct MovedFileFinding
{
    std::string expectedPath;  // as the first row spells it, absolute
    std::vector<Track> rows;   // every catalog's row for it, missing in each
    // High first, then by path. Empty when nothing on the stick matches.
    std::vector<MovedFileCandidate> candidates;

    // The one candidate when there is exactly one, else nullptr: the only
    // shape a repair is offered for. Several candidates is a guess, and
    // guessing which file is which is the one thing this never does.
    const MovedFileCandidate *only() const;
    // Exactly one candidate, at high confidence: the one-click case.
    bool oneClick() const;
    // Exactly one candidate that a person has to confirm.
    bool oneToConfirm() const;
    // Title and artist of the track, from whichever row has them.
    std::string title() const;
    std::string artist() const;
};

// Pairs the rows LibraryConsistencyChecker found missing with the files
// findUnreferencedFiles() found no catalog naming (#103). Pure: no
// filesystem access. `missingRows` are the broken rows of every catalog
// together, each tagged with its format; `filesOnDisk` are the stray
// files as Tracks (format "disk"); `pathKey` is application::
// normalizedPathKey, which groups the rows of one file across catalogs
// that spell its path differently.
//
// Tiers, as the Stray File Statistics page tiers its own answer
// (docs/deduplication-roadmap.md): never averaged into one number.
//   High            the same filename and the same size as the catalog
//                   recorded (Track::catalogFileBytes).
//   NeedsConfirming the same filename with another size, or a size the
//                   catalog does not record; or another filename whose
//                   tags give the same title and artist and whose length
//                   is within `durationToleranceSeconds` (matchTracks'
//                   own fallback, MatchingPolicy::exactMatchSeconds()).
// A file that is a candidate for two findings is NeedsConfirming for
// both: it can go back to one place only, and which is a person's call.
//
// Findings come back in the order the rows came, one per expected path,
// those with no candidate included: "N tracks point at a file that has
// moved; found on the stick: M" counts both.
std::vector<MovedFileFinding> findMovedFiles(const std::vector<Track> &missingRows,
                                             const std::vector<Track> &filesOnDisk,
                                             const std::function<std::string(const std::string &)> &pathKey,
                                             double durationToleranceSeconds);

// The same with MatchingPolicy::exactMatchSeconds() as the tolerance.
std::vector<MovedFileFinding> findMovedFiles(const std::vector<Track> &missingRows,
                                             const std::vector<Track> &filesOnDisk,
                                             const std::function<std::string(const std::string &)> &pathKey);

}  // namespace seabass::domain
