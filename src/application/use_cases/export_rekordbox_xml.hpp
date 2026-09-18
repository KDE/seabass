// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::application
{

// Builds a rekordbox collection XML out of the catalogs on a stick.
//
// The point of this use case is the cue merge. A stick's three catalogs
// are one library written three times, and on a real one they disagree
// wildly about cues: the WHALESHARK stick carries 110 cues on the
// rekordbox side and 1781 on the Engine side, because the DJ cues on
// Denon gear. Exporting rekordbox's own view would therefore throw away
// nearly every cue the DJ has ever set. Collapsing the catalogs into
// files first (collapseCatalogRows, whose cue rule is union-across-rows)
// is what carries the Engine cues over, and it is the one step that must
// not be skipped for convenience.
//
// Everything here is read-only: rows in, a string out.
struct ExportRekordboxXmlOptions
{
    // Lower-case, no leading dot ("flac"). A track whose file has one of
    // these extensions is left out of the collection entirely.
    //
    // This exists for a hardware reason rather than a technical one:
    // rekordbox itself reads FLAC perfectly well and will happily export
    // it to a stick, but an XDJ-RX2 cannot play it, so a library meant
    // for that player has to be told to leave it behind. The FLAC files
    // stay on disk; they are simply not in this collection.
    std::vector<std::string> excludeExtensions;

    // Rewrites the path a catalog recorded into the path the file now
    // lives at, longest-prefix-wins. A stick says
    // "/Volumes/WHALESHARK/Contents/..."; the library on the computer says
    // "/Users/sebas/Music/WhalesharkLibrary/Tracks/...". A track whose
    // path matches no prefix keeps the path it came with.
    std::map<std::string, std::string> pathPrefixMap;

    // Cues inside the first second are dropped (domain::isJunkCue), whatever
    // kind they are -- a loop is the exception, because an intro loop on the
    // first bar is real work and carries an end as well as a start.
    bool dropJunkMemoryCues = true;

    // Tracks whose file path no catalog resolved cannot be written --
    // rekordbox keys its whole collection on Location. They are counted
    // and, with this set, listed individually.
    bool reportUnresolved = false;
};

// Two catalogs disagreeing about where the same cue sits.
//
// A hot cue slot holds exactly one cue on the hardware, so the merge
// cannot keep both: the first catalog in `rows` wins the slot (see
// domain::LocalRestorePlanner::mergeCues) and the other position is
// dropped. Reporting it is therefore not a nicety -- it is the only
// record that something was discarded, and it tells the caller that its
// own catalog order made the choice. Resolving it for real means writing
// to the stick, which is sync's job, not this one.
struct CueConflict
{
    std::string filePath;
    std::string title;
    std::string artist;
    std::string slot;  // "hot 3" or "memory"
    std::string formatA;
    double positionMsA = 0.0;
    std::string formatB;
    double positionMsB = 0.0;
};

struct ExportRekordboxXmlResult
{
    std::string xml;

    std::size_t tracksWritten = 0;
    std::size_t tracksExcludedByExtension = 0;
    std::size_t tracksWithoutPath = 0;
    std::size_t tracksStreaming = 0;
    std::size_t cuesWritten = 0;
    std::size_t junkMemoryCuesDropped = 0;
    std::size_t playlistsWritten = 0;
    // Cues that came from a catalog other than rekordbox's own -- the
    // headline number for "did the Denon cues make it across".
    std::size_t cuesFromOtherCatalogs = 0;

    std::vector<CueConflict> conflicts;
    std::vector<std::string> unresolvedPaths;
};

class ExportRekordboxXml
{
public:
    // `rows` is every catalog's rows concatenated, in the caller's own
    // fixed order (rekordbox, then OneLibrary, then Engine, as the CLI
    // does). Order matters: collapseCatalogRows keeps the first row for a
    // file as the base and fills gaps from the rest, so the leading
    // catalog decides which title/artist spelling wins.
    ExportRekordboxXmlResult execute(const std::vector<domain::Track> &rows,
                                      const ExportRekordboxXmlOptions &options = {});
};

}  // namespace seabass::application
