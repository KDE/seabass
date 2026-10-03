// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

#include "domain/cue_tolerance.hpp"
#include "domain/track.hpp"

namespace seabass::domain
{

// Engine has no memory cues. A track there holds eight hot-cue pads,
// eight saved-loop pads and one main cue (where CUE returns). Engine DJ
// itself, importing a rekordbox library, turns memory cues into hot cues
// on the free pads and memory loops into saved loops; Seabass does the
// same when it syncs onto Engine, so a track reads the same on a Prime
// as it would after Denon's own import.
//
// Seabass keeps one rule of its own: a pad number is either a cue or a
// loop, never both (AddCueController enforces it for edits), so cues and
// loops share the eight numbers here.
constexpr int EngineHotCuePads = 8;

struct EngineCueTranslation
{
    // What Engine holds: hot cues, saved loops, and at most one memory
    // cue, the main cue.
    std::vector<CuePoint> cues;
    // Memory cues and loops no pad was free for. Not written, and never
    // counted as missing from Engine.
    std::vector<CuePoint> leftOut;
};

// `cues` as Engine holds them. Hot cues and hot loops keep their pads.
// Memory cues, then memory loops, each in time order, take a free pad:
// the pad `existing` (Engine's current cues for the track) already holds
// that cue on when it is still free, so a sync does not shuffle pads,
// else the lowest free one. A memory cue at the place of one of the hot
// cues is on a pad already and takes no second one (rekordbox often has
// both, the pad made from the marker). Pads full: leftOut. The main cue
// is Engine's existing one when it has one, else the earliest memory cue
// it can hold (engineCanHoldMemoryCue): a cue point set on the player is
// never moved by a sync.
//
// A pad takes the memory cue's colour. rekordbox reports an uncoloured
// memory cue as black ("#000000"); that is no colour, and the pad is
// left to Engine's default rather than painted black.
//
// Positions compare within `toleranceMs` (domain/cue_tolerance.hpp).
EngineCueTranslation translateCuesForEngine(const std::vector<CuePoint> &cues,
                                            const std::vector<CuePoint> &existing,
                                            double toleranceMs = CueFallbackToleranceMs);

// Engine's cues read in the terms of a catalog with memory cues, `cues`
// being that catalog's own. An Engine hot cue or loop on the same pad at
// the same place as one of its hot cues is that hot cue; one at the
// place of one of its memory cues or loops is that memory cue's
// translation and comes back as it; every other is Engine's own; the
// main cue is a memory cue.
//
// Taking a pad at a memory cue's place for that memory cue's translation
// is what Engine DJ's older rekordbox import measurably did, but only on
// the pad that import gives it: the free pads in time order, the way
// translateCuesForEngine() does with no existing cues. A pad at that place
// with another number (one the catalog's own hot cue holds elsewhere, or
// a second pad for a memory cue already under a hot cue) could as well be
// a hot cue the DJ set there on the player. It is still read as the
// translation, so counts stay as they were, and listed in `uncertain` for
// the planner to ask about rather than assume. A saved loop is matched to
// a memory loop by its start; one that ends elsewhere is listed in
// `loopEnds`.
struct CuesFromEngine
{
    std::vector<CuePoint> hotCues;     // Engine's own hot cues and loops
    std::vector<CuePoint> memoryCues;  // translations found on pads, as memory cues, and the main cue
    struct Uncertain
    {
        CuePoint pad;     // the Engine pad
        CuePoint memory;  // the memory cue it sits at
    };
    std::vector<Uncertain> uncertain;
    // A saved loop at the start of one of the catalog's memory loops,
    // ending elsewhere: read as that memory loop's translation, whose
    // length was changed on one side. Listed for the planner to ask about
    // (SyncPlan::Reason::LoopEndsDiffer); memoryCues holds the catalog's
    // own loop, so nothing else counts it as different.
    std::vector<Uncertain> loopEnds;
};

CuesFromEngine cuesFromEngine(const std::vector<CuePoint> &engineCues, const std::vector<CuePoint> &cues,
                              double toleranceMs = CueFallbackToleranceMs);

// cuesFromEngine() as one list, the way a page counts it: a memory cue
// that is on a pad and is the cue point is one cue, not two. For saying
// what a sync onto Engine does in the other side's own terms: "adds 1
// memory cue", not "adds 1 hot cue and 1 memory cue".
std::vector<CuePoint> cuesInTermsOf(const std::vector<CuePoint> &engineCues, const std::vector<CuePoint> &cues,
                                    double toleranceMs = CueFallbackToleranceMs);

}  // namespace seabass::domain
