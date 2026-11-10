// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "domain/cue_tolerance.hpp"
#include "domain/track.hpp"

namespace seabass::domain
{

// Shared low-level helpers used by both intra-library duplicate detection
// (DuplicateTrackFinder) and cross-format track matching (TrackMatcher,
// and the local-machine cue restore path).

// Lowercases and strips whitespace, so "Song.mp3" and "song.mp3 " compare
// equal.
std::string normalizeFilename(const std::string &filename);

// Tracks under different filenames are still the same song if title+artist
// match exactly -- filenames can differ across formats/re-imports (e.g. a
// filename that embeds a playlist index), so this is the primary signal
// for matching the same underlying song across libraries. Empty
// title/artist is too weak to match on, so those return nullopt rather
// than colliding on an empty key.
std::optional<std::string> titleArtistKey(const Track &track);

// Pairs up tracks from two lists believed to be the same underlying song.
// Exact resolved file path is checked first and, when both sides have
// one, is decisive on its own -- the two rows describe the same physical
// file on the same stick regardless of which catalog's database is
// describing it, the strongest signal available and immune to metadata
// quality issues on either side. Falling back (when a file path is
// missing on one side, e.g. a broken row) to title+artist as the primary
// metadata signal (see titleArtistKey), then filename when either side
// is missing title/artist metadata, then requiring duration to match
// within a small tolerance either way. At most one match per `a` track,
// since callers use this to propagate a single track's cues, not to
// build a full many-to-many mapping.
//
// Returned pointers stay valid exactly as long as `a` and `b` do --
// callers matching across formats (TrackMatcher) or against a local
// cue backup (the restore path) both consume the result immediately,
// well within that scope.
// Whether a row that has its file may still be matched to another file by
// name. Across sticks it must: a track stored from one stick is the same
// song on another under a different path, which is what Metadata Backup
// and Restore match (AcrossLibraries, the default). Within one stick it
// must not: there a row whose file is present IS that file, and a catalog
// that does not list it has nothing to match (OneStick, what Sync Cue
// Points uses). Sync once paired an Engine-only third copy of a track
// with OneLibrary's row for a different copy that way and wrote one
// file's cues onto the other (WHALESHARK2, 2026-10-02).
enum class MatchScope { AcrossLibraries, OneStick };

// Which spellings of a path open one file on the stick (#86). FAT32 and
// exFAT ignore letter case, so a catalog may list "Akki (De)/x.mp3" for
// the file another lists as "AKKI (DE)/x.mp3" (SHAKEDOWN_8's OneLibrary
// beside its export.pdb and Engine), and the exact path does not pair them.
// A case-sensitive filesystem can hold both as two files, so a shared key
// is only where to look: the filesystem says whether it is one file.
//
// Both injected, because the domain neither keys paths nor reads disks:
// pathKey is application::normalizedPathKey, sameFile asks the filesystem
// and answers false when it cannot tell (a file not there).
struct StickFiles
{
    std::function<std::string(const std::string &)> pathKey;
    std::function<bool(const std::string &, const std::string &)> sameFile;
};

// With `files`, a row whose exact path finds no partner is paired with the
// one row on the other side whose path has the same key, when the key
// names exactly one row on each side and the filesystem says the two are
// one file. Anything else under a shared key (two rows on a side, two
// files) is left as the exact path left it, never picked between.
std::vector<std::pair<const Track *, const Track *>> matchTracks(const std::vector<Track> &a,
                                                                   const std::vector<Track> &b,
                                                                   MatchScope scope = MatchScope::AcrossLibraries,
                                                                   const StickFiles *files = nullptr);

// True if two cue sets are the same, ignoring order and allowing a small
// position tolerance (cross-format conversions round, see
// domain/cue_tolerance.hpp). Slot, kind and place decide (sameCuePlace: a
// loop only equals a loop with the same in and out point); comment and
// colour never do. Pass cueToleranceMsFor() of the two tracks' tempos:
// half a beat when they agree on one, the fallback otherwise, which is
// also the default for a caller with no tempo at all.
bool cueSetsEqual(const std::vector<CuePoint> &a, const std::vector<CuePoint> &b,
                  double toleranceMs = CueFallbackToleranceMs);

// The same cues, with any colour the target already had put back where
// the incoming cue has none.
//
// Colour is the one part of a cue that each format keeps its own way:
// Engine's memory cue has none at all, an un-set pad reads as no colour,
// and a writer that cannot record one leaves it empty. So a cue coming
// the other way with an empty colour is not someone saying "make this
// one grey" -- it is silence, and silence must not overwrite what is
// already there. A cue that does carry a colour brings it with it.
//
// Matched the way cueSetsEqual matches: hot cues by slot (a loop with a
// loop), memory cues by place within the same tolerance (sameCuePlace).
std::vector<CuePoint> keepExistingColours(std::vector<CuePoint> incoming,
                                          const std::vector<CuePoint> &existing,
                                          double toleranceMs = CueFallbackToleranceMs);

}  // namespace seabass::domain
