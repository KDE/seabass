// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

namespace seabass::infrastructure::engine
{

// How many tracks an Engine player will have to analyse the first time
// they are loaded (issue #38).
//
// A library that came from Engine DJ's own "import rekordbox library"
// carries the rows but not the analysis: isAnalyzed = 0, with NULL in
// trackData, overviewWaveFormData and beatData, while quickCues and
// loops still hold the cues the import brought over. Measured on a
// Denon-written stick: 1219 of 1566 tracks in that state, and 1214 of
// 1564 in this repository's own fixture.
//
// That is the design, not a fault. Load such a track on a Prime 4 and it
// analyses it there and then, writes the result back to the stick, and
// every load after that is instant. The cost is simply paid at the worst
// possible moment -- the first load waits for it, at a gig, on a deck,
// in front of people -- and nothing tells anybody the stick is in that
// state, so nobody warms it up beforehand.
//
// Counted on isAnalyzed rather than on the three blobs being NULL,
// because isAnalyzed is what the PLAYER reads to decide whether to
// analyse. Those normally agree, and where they disagree the flag is
// right: this repository's fixture holds two tracks with isAnalyzed = 0
// and full-sized analysis blobs, which the player will re-analyse on
// load. Counting NULL blobs would miss exactly the tracks that look done
// and are not.
//
// Seabass deliberately does not offer to do the analysis. Producing
// those blobs means reproducing Denon's beatgrid, waveform and key
// detection, and anything generated here would be a worse approximation
// that also stops the player ever doing it properly -- see
// markTracksForDeviceAnalysis() in libdjinterop_engine_library_creator,
// which sets isAnalyzed = 0 on a created library on purpose, for the
// same reason. A library Seabass has just created therefore reports
// 100% unanalysed, correctly.

// Deliberately its own audit rather than a field on domain::Track.
//
// Analysis state is DEVICE state, not library content: the player flips
// isAnalyzed from 0 to 1 and rewrites lastEditTime just by loading a
// track, with nothing about the library having changed. A Track field
// would sooner or later reach application::catalogDigestLines(), which
// the release rig uses to tell a SQLite checkpoint from a real change --
// and then every rig round where somebody loaded a track on a Prime 4
// would report the catalogs as differing. The digest excludes everything
// derived or device-dependent for exactly this reason; the cheapest way
// to keep it excluded is for it never to be a Track field at all.
struct AnalysisStateAudit
{
    bool libraryPresent = false;  // there is an Engine database here at all
    int tracksChecked = 0;        // rows in Track
    int notAnalyzed = 0;          // of those, isAnalyzed = 0
    bool hasColumn = false;       // false on Engine 1.x, which has no such column
    std::string error;            // the library could not be read at all

    // Worth telling the user about. Nothing to say when there is no
    // library, or it is empty, unreadable, or too old to record the
    // state -- and those are four different sentences, not one. A stick
    // with no Engine library was being told its Engine library does not
    // record analysis state, which is a claim about something that is
    // not there.
    bool worthReporting() const { return error.empty() && hasColumn && notAnalyzed > 0; }
};

// Reads <engineLibraryPath>/Database2/m.db. An absent library is not an
// error: it reports nothing found, the way the other Engine audits do.
AnalysisStateAudit auditAnalysisState(const std::string &engineLibraryPath);

}  // namespace seabass::infrastructure::engine
