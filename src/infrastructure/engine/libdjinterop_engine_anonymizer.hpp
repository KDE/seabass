// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/progress_reporter.hpp"

namespace seabass::infrastructure::engine
{

struct EngineAnonymizationResult
{
    int tracksKept = 0;
    int tracksDropped = 0;  // only nonzero when maxTracks was set and exceeded
    int playlistsRenamed = 0;
    // Files in Database2 that no anonymizer scrubs, dropped rather than
    // shipped. hm.db -- the play history, with real titles, artists and
    // paths -- was going out in every export until this existed.
    std::vector<std::string> removedUnanonymizableFiles;
    // PerformanceData rows whose waveform blob was emptied, when
    // slimForTesting was asked for. The cues in the same row are kept.
    int waveformRowsEmptied = 0;
    // Tracks libdjinterop refused to read or write (undecodable
    // performance data, which real libraries genuinely contain). Their
    // metadata is NOT anonymized, so a nonzero count here means the export
    // must not be shared -- the caller is expected to say so loudly.
    // Rows whose stale Track.filename column was rewritten from the
    // anonymized path (libdjinterop has no setter for it).
    int filenameColumnRows = 0;
    int tracksRefused = 0;
    std::string firstRefusalReason;
    std::string errorMessage;  // empty on success
};

// Produces an obfuscated copy of a real Engine Library at
// destinationRoot (created fresh -- refuses if it already exists):
// copies sourceRoot's Database2/ only (not Artwork/, dropped entirely),
// opens the *copy* via djinterop::engine::load_database(), and mutates
// it in place -- title/artist/comment/relative_path on every kept
// track (djinterop::track's own live setters, backed directly by the
// row -- no byte-length constraint the way rekordbox's DeviceSQL format
// has, since this is SQLite), every hot cue's/loop's label (the
// Engine-side equivalent of a rekordbox cue comment -- real free text a
// DJ typed per cue point, not just position/color), and every
// playlist/folder's name (whole tree, via root_playlists()/children()).
//
// Deliberately mutates the real database in place rather than
// rebuilding one via EngineLibraryCreator: that class doesn't carry
// over playlists yet, and the goal here is testing against a library as
// close to what real Engine DJ software actually produces as possible,
// not one this app generated.
//
// If maxTracks is set and the library has more real tracks than that,
// prunes the excess (in db.tracks()'s own order) via
// database::remove_track() -- the same real, tested primitive
// LibdjinteropEngineCleanupWriter already relies on -- after first
// removing each doomed track from every playlist's track list it
// appears in (no survivor to repoint to, unlike that class's
// consolidation use case). Otherwise every real track is kept.
//
// sourceRoot/destinationRoot are both "engineLibraryPath" paths -- the
// directory directly containing Database2/, matching
// LibdjinteropEngineReader's own convention.
EngineAnonymizationResult anonymizeEngineLibrary(
    const std::string &sourceRoot, const std::string &destinationRoot, std::optional<size_t> maxTracks,
    // See AnonymizationOptions::slimForTesting: empties the waveform
    // blob and drops the .rgb previews, keeping the cues beside them.
    bool slimForTesting = false,
    application::ProgressReporter &reporter = application::NullProgressReporter::instance());

}  // namespace seabass::infrastructure::engine
