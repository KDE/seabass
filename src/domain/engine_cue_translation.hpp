// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

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
// else the lowest free one. Pads full: leftOut. The main cue is Engine's
// existing one when it has one, else the earliest memory cue it can hold
// (engineCanHoldMemoryCue): a cue point set on the player is never moved
// by a sync.
EngineCueTranslation translateCuesForEngine(const std::vector<CuePoint> &cues,
                                            const std::vector<CuePoint> &existing);

// Engine's cues read in the terms of a catalog with memory cues.
// `memoryCues` are that catalog's own memory cues and loops: an Engine
// hot cue or saved loop at one of their positions is that memory cue's
// translation and comes back as it, not as a hot cue; every other hot
// cue and loop is its own; the main cue is a memory cue.
struct CuesFromEngine
{
    std::vector<CuePoint> hotCues;     // Engine's own hot cues and loops
    std::vector<CuePoint> memoryCues;  // translations found on pads, as memory cues, and the main cue
};

CuesFromEngine cuesFromEngine(const std::vector<CuePoint> &engineCues, const std::vector<CuePoint> &memoryCues);

}  // namespace seabass::domain
