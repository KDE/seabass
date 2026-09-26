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

// Whether a cached length is checked against its file before it is used.
// Verified looks at every file it takes a length for (a stat, via
// DurationCachePort::lookup); Unverified takes it by path alone and lists
// the files it did that for, so a later pass can check them once the
// stick has been read further (verifyCachedDurations()).
enum class CachedDurations { Verified, Unverified };

struct FillMissingDurationsResult
{
    size_t alreadyKnown = 0;  // the catalog had a length; never re-probed
    size_t fromCache = 0;     // served from the stick's duration cache
    size_t probed = 0;        // read from the audio file this run
    size_t unreadable = 0;    // no length available even after probing

    // The four add up to the number of tracks that went in, always.
    // Rows after the first pointing at one file are filled from what
    // this run already worked out for it, and are counted under where
    // that came from rather than not at all: several rows per file is
    // the ordinary shape here, since duplicates are what this feeds.

    // Of fromCache, the rows whose length was taken without looking at
    // the file (CachedDurations::Unverified), and the distinct files they
    // name, for verifyCachedDurations(). Always 0 and empty when Verified.
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
                                                 CachedDurations trust = CachedDurations::Verified,
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

// The check a CachedDurations::Unverified fill owes: for each of `paths`
// (FillMissingDurationsResult::unverifiedPaths), one verified lookup;
// where the file no longer matches the cache, a fresh probe, stored, and
// every row naming that file that still carries the stale cached length
// gets the new one. A row with a length the catalog gave was never filled
// from the cache and is left alone, unless its length is the stale value
// to the last bit, where it gets the same correction. `cancel` as above.
VerifyCachedDurationsResult verifyCachedDurations(std::vector<domain::Track> &tracks,
                                                   const std::vector<std::string> &paths, TrackDurationProbe &probe,
                                                   DurationCachePort &cache,
                                                   CancellationToken cancel = CancellationToken::none());

}  // namespace seabass::application
