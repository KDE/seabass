// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// One cue sitting at the very start of a track -- noise rather than a
// marker anyone placed. The track already starts at 0:00, so a cue there
// navigates to nothing; what it comes from is a stray press during
// analysis, an import artifact, or a format's own "no cue set" sentinel.
//
// Hot cues at 0:00 counted as deliberate until 2026-09-18, on the theory
// that some DJs keep a "track start" pad. Real sticks say otherwise: they
// turn up one or two to a library, at 7 ms and 109 ms, indistinguishable
// from the memory-cue noise beside them and just as useless to navigate
// by. They are cleaned up with the rest now.
//
// A position before the start counts too, and is the plainer case: it
// cannot be a cue anyone placed. Seabass made those itself until the
// Engine reader learned that a main_cue of -1 means "no cue set" rather
// than a position (libdjinterop_engine_reader.cpp), and sticks and
// metadata backups written before that still carry them, at minus a
// fraction of a millisecond.
//
// "At the start" means positionMs < 1000 (displays as "0:00" in every
// mm:ss field this app has, all of which floor to whole seconds -- see
// e.g. PlayerBar.qml's formatTime()), not literally positionMs == 0.
// Real Engine data confirmed this matters: Engine's own auto-generated
// main_cue routinely lands a few hundred ms into the track (its analysis
// picks the first detected beat/transient, not sample 0 exactly) --
// real examples seen include 286ms, 339ms, 539ms, and 750ms, none of
// which the old exact-zero check caught, so genuinely stray cues a user
// could see displayed as "0:00" were silently never offered for
// cleanup.

struct JunkCueIssue
{
    Track track;    // the track carrying the cue
    CuePoint cue;   // the specific cue at the start
    // Why this cue is here, for the row that offers to delete it. Two
    // checks feed this list now -- the first-second rule below and the
    // clustered-hot-cue rule in clustered_cue.hpp -- and a user being
    // asked to delete somebody's cue is owed the reason rather than one
    // wording that covers both badly.
    std::string reason;
};

// Pure, no filesystem/database access -- callers already have a fresh
// track list from the same scan that also feeds
// LibraryConsistencyChecker, this just looks at cues directly rather
// than file existence.
// The one definition of "junk": any cue inside the first second, hot or
// memory, except a loop. A loop starting on the first bar is a real
// thing someone set, and it carries an end as well as a start, which a
// stray press never does. Shared by the finder and the remover so the
// two cannot disagree.
//
// Preferences -> Music -> "Ignore cues at 0:00" turns the first-second
// rule off for a DJ who does keep a "track start" pad, in which case
// nothing here is junk except a cue at a negative position -- that one
// is a "no cue set" sentinel read as a position and points nowhere at
// all, so it stays junk either way.
bool isJunkCue(const CuePoint &cue);

// The same cues with the junk left out.
//
// A stray cue is a fault to be cleaned off a stick, not a piece of a
// DJ's work: it must not be copied into a metadata backup, must not
// count towards "which side has more cues" when the two disagree, and
// must never be written back onto a stick by a restore -- which would
// put back exactly what Library Health had just taken off.
std::vector<CuePoint> withoutJunkCues(const std::vector<CuePoint> &cues);

// rekordbox's export puts a hot cue at 0:00 on almost every track
// (Sebastian, 2026-10-10: "Yet again, I saw rekordbox putting a 0:00 cue
// on almost every track"). Where Engine holds a real cue on that pad, the
// 0:00 cue is export noise over it, not a change rekordbox made: it is
// never a conflict and never removes or moves Engine's cue. Engine's cue
// goes back onto rekordbox over it, checked. Both cue planners decide the
// case through this one function, so Sync Cue Points
// (SyncPlanner::plan) and Sync after Rekordbox Export
// (EngineUpdatePlanner::plan) cannot drift apart.
//
// One entry per pad, 1 to 8, where `rekordboxCues` (a catalog's cues as
// scanned, junk included) holds a hot cue isJunkCue() calls junk and no
// other hot cue, and `engineHotCues` (Engine's own hot cues and loops, read
// in that catalog's terms: cuesFromEngine's hotCues) holds one that is not
// junk. Empty with "Ignore cues at 0:00" off: then a cue at 0:00 is a cue
// like any other and the planners compare it as one.
//
// A memory cue at 0:00 has no pad and is not covered: it is junk under the
// policy as before, left out of every comparison and every write. A loop
// is never junk (isJunkCue), so a hot loop on the first bar is a real pad.
// Where Engine holds nothing on the pad, or only junk, the 0:00 cue stays
// ignored as before and is not copied onto Engine.
struct StartCueOverEngine
{
    int pad = 0;
    CuePoint startCue;   // rekordbox's cue at the start, on `pad`
    CuePoint engineCue;  // Engine's cue on `pad`, what goes back
};

std::vector<StartCueOverEngine> startCuesOverEngine(const std::vector<CuePoint> &rekordboxCues,
                                                    const std::vector<CuePoint> &engineHotCues);

// rekordboxCues with the start cues `over` names taken off their pads and
// Engine's cues there put in their place: what rekordbox holds once
// Engine's go back.
std::vector<CuePoint> withEngineCuesOverStartCues(const std::vector<CuePoint> &rekordboxCues,
                                                  const std::vector<StartCueOverEngine> &over);

class JunkCueFinder
{
public:
    static std::vector<JunkCueIssue> find(const std::vector<Track> &tracks);
};

}  // namespace seabass::domain
