// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/progress_reporter.hpp"

namespace seabass::infrastructure::rekordbox
{

struct RekordboxAnonymizationResult
{
    int tracksKept = 0;
    int tracksDropped = 0;  // only nonzero when maxTracks was set and exceeded
    int artistsRenamed = 0;
    int playlistsRenamed = 0;
    // Analysis files removed because no kept track pointed at them, when
    // pruneUnreferencedAnalysisFiles was asked for.
    int orphanedAnalysisFilesRemoved = 0;
    // Files found in the catalog directory that no anonymizer knows how
    // to scrub, and were therefore dropped rather than shipped. Reported
    // so the manifest can say what is missing from the export instead of
    // leaving a submitter to wonder.
    std::vector<std::string> removedUnanonymizableFiles;
    // Analysis files scrubbed that no present track row pointed at:
    // leftovers from tracks deleted from the library, which the copy
    // brings along and which still carry their real path.
    int orphanedAnalysisFilesScrubbed = 0;
    // Player-preference files copied verbatim: Device Profile is the only
    // thing that reads them and they carry nothing identifying.
    int deviceSettingsFilesCopied = 0;
    // Rows scrubbed in the Device Library Plus mirror that lives beside
    // export.pdb, and why it could not be scrubbed if it could not.
    int oneLibraryTracksScrubbed = 0;
    std::string oneLibraryError;
    std::string errorMessage;  // empty on success
};

// Produces an obfuscated copy of a rekordbox USB export at
// destinationRoot (created fresh -- refuses if it already exists):
//
//  - copies sourceRoot's rekordbox/ (export.pdb) and USBANLZ/ trees
//    only -- not Artwork/ (dropped entirely, see below) and not the
//    misc CDJ/rekordbox device-settings files (MYSETTING*.DAT,
//    DJMMYSETTING.DAT, DEVSETTING.DAT, djprofile.nxs, CDJ/, MPJ/, log/,
//    TrashBox/, extracted/), which aren't read by any of this app's
//    library code and (djprofile.nxs especially) can carry
//    device-identifying content this has no reason to include.
//  - if maxTracks is set and the library has more real tracks than
//    that, prunes down to the first maxTracks (in on-disk order) via
//    PdbRowWriter::removeTrack/removePlaylistEntry -- the format's own
//    presence-bit deletion, so no structural rewrite -- and deletes the
//    corresponding dropped tracks' USBANLZ/<hash>/ subtrees entirely.
//    Otherwise every real track is kept.
//  - overwrites every *kept* track's title/comment/filename/file_path,
//    every artist's name (once per distinct artist, since many tracks
//    typically share one), and every playlist/folder's name, in place
//    via PdbRowWriter -- byte-length-preserving, see its own doc
//    comment for why this never resizes or reflows a row.
//  - for every kept track's ANLZ .DAT/.EXT/.2EX files, strips every
//    large waveform-detail section this app's own reader never touches
//    (WAVE_SCROLL/WAVE_COLOR_PREVIEW/WAVE_COLOR_SCROLL/WAVE_3BAND_
//    PREVIEW/WAVE_3BAND_SCROLL -- see rekordbox_library_anonymizer.cpp's
//    own comment for why WAVE_SCROLL belongs in this list too, found by
//    checking real captured files rather than trusting the spec's field
//    list alone) and obfuscates every cue's comment text (real free
//    text a DJ may have typed per cue point -- see
//    kaitai_rekordbox_reader.cpp's readCues(), which does read it from
//    real files even though this project's own cue writer
//    doesn't write new ones), keeping cue positions/colors/beatgrid/
//    path/monochrome-preview untouched.
//
// sourceRoot/destinationRoot are both "pioneerRoot" paths -- the
// directory directly containing rekordbox/ and USBANLZ/, matching
// KaitaiRekordboxReader's own convention (see its header comment).
RekordboxAnonymizationResult anonymizeRekordboxLibrary(
    const std::string &sourceRoot, const std::string &destinationRoot, std::optional<size_t> maxTracks,
    // See AnonymizationOptions::slimForTesting: removes the analysis
    // files no kept track points at rather than scrubbing and shipping
    // them.
    bool slimForTesting = false,
    application::ProgressReporter &reporter = application::NullProgressReporter::instance());

}  // namespace seabass::infrastructure::rekordbox
