// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "application/ports/track_duration_probe.hpp"
#include "application/use_cases/fill_missing_durations.hpp"
#include "domain/track.hpp"
#include "infrastructure/local/duration_cache.hpp"

#ifdef SEABASS_HAVE_QT_AUDIO
#include "infrastructure/audio/qt_multimedia_duration_probe.hpp"
#endif

namespace seabass::infrastructure::audio
{

// The one place that decides which TrackDurationProbe a composition root
// gets, so seabass-cli and the GUI cannot drift apart on it -- they did
// once already: the fill was wired into the CLI's scan path only, which
// left the GUI's LibraryCatalogCache::realScan() reading catalogs with
// no lengths and so finding *fewer* duplicates than before, the exact
// opposite of the intent.
//
// Header-only and #ifdef'd rather than a function in seabass_core,
// because choosing the Qt probe is precisely the part core must not
// know about. SEABASS_HAVE_QT_AUDIO is PUBLIC on seabass_audio_qt, so
// any target that links it compiles the real probe in and any target
// that does not falls back to the null one.
//
// libraryPath is the catalog directory (".../PIONEER", ".../Engine
// Library"); the cache lives beside it at the stick root, since the
// answers describe the stick's files rather than this machine. A
// libraryPath that is already the stick root is harmless: DurationCache
// ignores anything resolving outside the root it was given, so a wrong
// guess costs caching, never correctness.
inline application::FillMissingDurationsResult fillTrackDurations(std::vector<domain::Track> &tracks,
                                                                    const std::string &libraryPath)
{
    const std::string stickRoot = std::filesystem::path(libraryPath).parent_path().string();
    local::DurationCache cache(stickRoot);

#ifdef SEABASS_HAVE_QT_AUDIO
    QtMultimediaDurationProbe probe;
#else
    application::NullTrackDurationProbe probe;
#endif

    auto result = application::fillMissingDurations(tracks, probe, &cache);
    // A read-only or full stick costs only a re-probe next time, never
    // the scan itself.
    cache.save();
    return result;
}

}  // namespace seabass::infrastructure::audio
