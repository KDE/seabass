// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/duration_cache_port.hpp"
#include "application/ports/track_duration_probe.hpp"
#include "domain/track.hpp"

namespace seabass::application
{

// How far a fill goes for a track its catalog gave no length.
//
// Complete: the stick's cache, every entry checked against its file (a
// stat, via DurationCachePort::lookup), and a probe of the file for
// whatever the cache does not know. Everything there is to know.
//
// CachedByPathOnly: the cache by path alone, never looking at an audio
// file: no stat, no probe. For the catalog cache's Tracks stage, which
// must not touch the audio files (a stat per file is seconds cold on a
// stick, a probe far more). The files whose length it took are listed,
// for verifyCachedDurations() to check later; a track the cache does not
// know is left at 0 and counted as deferred, for a Complete fill to
// probe later.
enum class DurationFill { Complete, CachedByPathOnly };

struct FillMissingDurationsResult
{
    size_t alreadyKnown = 0;  // the catalog had a length; never re-probed
    size_t fromCache = 0;     // served from the stick's duration cache
    size_t probed = 0;        // read from the audio file this run
    size_t unreadable = 0;    // no length available even after probing
    size_t deferred = 0;      // not in the cache, and this fill does not probe (CachedByPathOnly)

    // The five add up to the number of tracks that went in, always.
    // Rows after the first pointing at one file are filled from what
    // this run already worked out for it, and are counted under where
    // that came from rather than not at all: several rows per file is
    // the ordinary shape here, since duplicates are what this feeds.

    // Of fromCache, the rows whose length was taken without looking at
    // the file (DurationFill::CachedByPathOnly), and the distinct files
    // they name, for verifyCachedDurations(). Always 0 and empty when
    // Complete.
    size_t fromCacheUnverified = 0;
    std::vector<std::string> unverifiedPaths;
};

// Fills `durationSeconds` on every track that has none, from the cache
// first and the probe second, and marks each row it filled
// `durationIsProbed` (the cache only ever holds probed lengths). Tracks that already have a length are left
// completely alone -- the catalog's own value wins, both because it is
// free and because re-deriving a value the catalog already agrees with
// would only invite drift.
//
// Why this is a use case and not something the readers do: the probe
// needs Qt, the readers live in the Qt-free core, and both catalogs need
// the same treatment. Doing it once here, after the read, keeps the
// dependency at the edge.
//
// `cancel` is checked before every file this looks up or probes: a
// cancelled fill throws OperationCancelled with the tracks half filled,
// which is why its callers work on a copy.
FillMissingDurationsResult fillMissingDurations(std::vector<domain::Track> &tracks, TrackDurationProbe &probe,
                                                 DurationCachePort *cache,
                                                 DurationFill fill = DurationFill::Complete,
                                                 CancellationToken cancel = CancellationToken::none());

struct VerifyCachedDurationsResult
{
    size_t confirmed = 0;   // the file is as it was probed: the length stands
    size_t reprobed = 0;    // the file changed, and was probed again
    size_t unreadable = 0;  // the file changed or went, and gave no length
    // Rows whose length this changed: to the new length, or to 0 (unknown)
    // for an unreadable file, as a Verified fill would have left them.
    size_t tracksUpdated = 0;
};

// The check a DurationFill::CachedByPathOnly fill owes: for each of
// `paths` (FillMissingDurationsResult::unverifiedPaths), one verified
// lookup; where the file no longer matches the cache, a fresh probe,
// stored, and every row naming that file that was filled in with the
// stale cached length (durationIsProbed) gets the new one. A row whose
// length the catalog gave is never touched. A file that changed and then
// gives no length (gone, or no longer audio) is forgotten by the cache,
// not left there with its stale length: otherwise every later Tracks
// stage would take the stale value by path and every Full stage turn it
// back into 0. `cancel` as above.
VerifyCachedDurationsResult verifyCachedDurations(std::vector<domain::Track> &tracks,
                                                   const std::vector<std::string> &paths, TrackDurationProbe &probe,
                                                   DurationCachePort &cache,
                                                   CancellationToken cancel = CancellationToken::none());

}  // namespace seabass::application
