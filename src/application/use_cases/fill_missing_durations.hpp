// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/duration_cache_port.hpp"
#include "application/ports/track_duration_probe.hpp"
#include "domain/track.hpp"

namespace seabass::application
{

struct FillMissingDurationsResult
{
    size_t alreadyKnown = 0;  // the catalog had a length; never re-probed
    size_t fromCache = 0;     // served from the stick's duration cache
    size_t probed = 0;        // read from the audio file this run
    size_t unreadable = 0;    // no length available even after probing
};

// Fills `durationSeconds` on every track that has none, from the cache
// first and the probe second. Tracks that already have a length are left
// completely alone -- the catalog's own value wins, both because it is
// free and because re-deriving a value the catalog already agrees with
// would only invite drift.
//
// Why this is a use case and not something the readers do: the probe
// needs Qt, the readers live in the Qt-free core, and both catalogs need
// the same treatment. Doing it once here, after the read, keeps the
// dependency at the edge.
FillMissingDurationsResult fillMissingDurations(std::vector<domain::Track> &tracks, TrackDurationProbe &probe,
                                                 DurationCachePort *cache);

}  // namespace seabass::application
