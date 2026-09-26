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
#include "infrastructure/paths/seabass_paths.hpp"

#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/taglib_duration_probe.hpp"
#endif
#ifdef SEABASS_HAVE_QT_AUDIO
#include "infrastructure/audio/qt_multimedia_duration_probe.hpp"
#endif

namespace seabass::infrastructure::audio
{

// Runs `use` with the probe this build has.
//
// TagLib first wherever it is compiled in: synchronous, header-only
// reads, and the same answer on every platform. The Qt probe needs a
// running event loop and a working platform backend, and on macOS
// QMediaPlayer never finishes loading at all -- every file cost its
// five-second timeout and returned nothing (found by the rig,
// 2026-09-17). Where neither is built the null probe keeps every
// caller's shape.
template<typename Use>
auto withDurationProbe(Use &&use)
{
#if defined(SEABASS_HAVE_TAGLIB)
    TagLibDurationProbe probe;
#elif defined(SEABASS_HAVE_QT_AUDIO)
    QtMultimediaDurationProbe probe;
#else
    application::NullTrackDurationProbe probe;
#endif
    return use(static_cast<application::TrackDurationProbe &>(probe));
}

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
//
// fill: Complete checks every cached length against its file (a stat
// each) and probes every file the cache does not know. CachedByPathOnly
// takes cached lengths by path and opens nothing, for the catalog cache's
// Tracks stage, which must not touch the audio files: it returns the
// files it took a length for in unverifiedPaths, for
// verifyTrackDurations() to check at the Full stage, and leaves a file
// the cache does not know at 0 (deferred) for the Full stage's Complete
// fill to probe.
// cancel: checked per file; a cancelled fill still saves what it probed.
inline application::FillMissingDurationsResult
fillTrackDurations(std::vector<domain::Track> &tracks, const std::string &libraryPath,
                   application::DurationFill fill = application::DurationFill::Complete,
                   application::CancellationToken cancel = application::CancellationToken::none())
{
    local::DurationCache cache(paths::stickRootForCatalogPath(libraryPath));
    return withDurationProbe([&](application::TrackDurationProbe &probe) {
        try {
            auto result = application::fillMissingDurations(tracks, probe, &cache, fill, cancel);
            // A read-only or full stick costs only a re-probe next time,
            // never the scan itself.
            cache.save();
            return result;
        } catch (...) {
            // What was probed before the cancel is still true.
            cache.save();
            throw;
        }
    });
}

// The check a CachedByPathOnly fillTrackDurations() owes, for its
// unverifiedPaths: one verified lookup per file, a fresh probe for a file
// that changed since it was cached, the rows that carried the stale
// length corrected, and the entry of a changed file that no longer gives
// a length forgotten. By the Full stage the sizes have been read, so the
// stat each file costs here is answered from the kernel's cache.
inline application::VerifyCachedDurationsResult verifyTrackDurations(
    std::vector<domain::Track> &tracks, const std::string &libraryPath, const std::vector<std::string> &unverifiedPaths,
    application::CancellationToken cancel = application::CancellationToken::none())
{
    if (unverifiedPaths.empty()) {
        return {};
    }
    local::DurationCache cache(paths::stickRootForCatalogPath(libraryPath));
    return withDurationProbe([&](application::TrackDurationProbe &probe) {
        try {
            auto result = application::verifyCachedDurations(tracks, unverifiedPaths, probe, cache, cancel);
            cache.save();
            return result;
        } catch (...) {
            cache.save();
            throw;
        }
    });
}

}  // namespace seabass::infrastructure::audio
