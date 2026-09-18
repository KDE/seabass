// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

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
bool isJunkCue(const CuePoint &cue);

// The same cues with the junk left out.
//
// A stray cue is a fault to be cleaned off a stick, not a piece of a
// DJ's work: it must not be copied into a metadata backup, must not
// count towards "which side has more cues" when the two disagree, and
// must never be written back onto a stick by a restore -- which would
// put back exactly what Library Health had just taken off.
std::vector<CuePoint> withoutJunkCues(const std::vector<CuePoint> &cues);

class JunkCueFinder
{
public:
    static std::vector<JunkCueIssue> find(const std::vector<Track> &tracks);
};

}  // namespace seabass::domain
