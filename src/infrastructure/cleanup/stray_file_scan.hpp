// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/track_metadata_probe.hpp"
#include "application/use_cases/find_unreferenced_files.hpp"
#include "application/use_cases/unreferenced_tracks.hpp"
#include "domain/track.hpp"
#include "infrastructure/cleanup/audio_file_walk.hpp"
#include "infrastructure/local/metadata_cache.hpp"

#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/taglib_metadata_probe.hpp"
#endif

namespace seabass::infrastructure::cleanup
{

// What a stray-file scan found, and -- just as important -- what it is
// entitled to claim. Every field here exists because the failure mode of
// this whole area is a partial answer that looks complete.
struct StrayFileScanResult
{
    // One per stray file the probe could read, ready to go through
    // DuplicateTrackFinder alongside the catalogued tracks.
    std::vector<domain::Track> tracks;

    std::vector<std::string> catalogsConsulted;
    std::size_t filesFound = 0;   // strays on disk, before probing
    std::uint64_t bytesFound = 0;
    std::size_t unreadable = 0;   // found, but no metadata could be read

    // At least one directory could not be read, so there may be more
    // files than were seen. Not a safety problem (an unseen file is
    // never proposed) but the totals must not be presented as the whole
    // truth -- see AudioFileWalkResult::incomplete.
    bool walkIncomplete = false;

    // False when this build has no TagLib: every file then reads as
    // unreadable, and "632 files found, none reviewable" needs saying
    // rather than showing as an empty list.
    bool metadataProbeAvailable = false;

    // False means offer nothing at all, and show `refusal` instead of a
    // count. A stray-file answer is only as good as the set of catalogs
    // it was subtracted from.
    bool usable = false;
    std::string refusal;
};

// Finds the audio files on `stickRoot` that none of `catalogs`
// references, and turns them into Tracks.
//
// `failedCatalogs` names catalogs whose database is present on the stick
// but could not be read (gui::StickCatalogRead::failed). Any entry there
// refuses the whole scan: a file the unreadable catalog protects would
// otherwise be presented as unreferenced, and the review it then enters
// ends in deleting it. Absent is fine, unreadable is not -- the two are
// deliberately not the same thing.
//
// Header-only and #ifdef'd for the same reason as
// infrastructure/audio/duration_fill.hpp: which TrackMetadataProbe a
// composition root gets is precisely the choice seabass_core must not
// make, and doing it in one place is what keeps the GUI and the CLI from
// drifting apart on it (they did once already, see that header).
inline StrayFileScanResult scanStrayFiles(const std::string &stickRoot, const application::CatalogTracks &catalogs,
                                            const std::vector<std::string> &failedCatalogs,
                                            const application::CancellationToken &cancel)
{
    StrayFileScanResult result;
#ifdef SEABASS_HAVE_TAGLIB
    result.metadataProbeAvailable = true;
#endif

    if (!failedCatalogs.empty()) {
        std::string names;
        for (const auto &name : failedCatalogs) {
            names += (names.empty() ? "" : ", ") + name;
        }
        result.refusal = "This stick has a " + names
                         + " database that could not be read, so which files it still needs is unknown. Files "
                           "no catalog references are not listed.";
        return result;
    }

    auto walk = walkAudioFiles((std::filesystem::path(stickRoot) / "Contents").string(), cancel);
    auto scan = application::findUnreferencedFiles(walk.files, catalogs);
    if (!scan.usable) {
        result.refusal = "No catalog on this stick could be read, so nothing can be called unreferenced.";
        return result;
    }

    result.usable = true;
    result.walkIncomplete = walk.incomplete;
    result.catalogsConsulted = scan.catalogsConsulted;
    result.filesFound = scan.unreferenced.size();
    for (const auto &f : scan.unreferenced) {
        result.bytesFound += f.fileSizeBytes;
    }

    local::MetadataCache cache(stickRoot);
#ifdef SEABASS_HAVE_TAGLIB
    audio::TagLibMetadataProbe probe;
#else
    application::NullTrackMetadataProbe probe;
#endif
    result.tracks = application::unreferencedFilesAsTracks(scan.unreferenced, probe, &cache, cancel);
    // A read-only or full stick costs a re-read next time, never the scan.
    cache.save();
    result.unreadable = result.filesFound - result.tracks.size();
    return result;
}

}  // namespace seabass::infrastructure::cleanup
