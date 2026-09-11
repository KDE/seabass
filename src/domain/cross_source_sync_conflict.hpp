// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

#include "domain/sync_planning.hpp"
#include "domain/track.hpp"

namespace seabass::domain
{

// Two different SyncPlans -- each from a DIFFERENT pairwise comparison
// (rekordbox<->Engine, rekordbox<->OneLibrary, Engine<->OneLibrary, see
// application::SyncLibraries) -- that both propose writing cues to the
// exact same physical track in a third catalog, and whose proposals
// actually disagree. Neither pairwise SyncPlanner can detect this on its
// own: it only ever sees two catalogs at a time, so it has no way to know
// a third pair independently wants to write something different to the
// same target. See CrossSourceConflictDetector::detect().
//
// Deliberately distinct from SyncPlan::Kind::Conflict, which is a
// different, already-existing concept: one pair's two sides disagreeing
// with each other, auto-resolved by last-write-wins on file mtime (a
// heuristic, but at least a real signal). This conflict has no such
// signal to lean on -- two independent *sources* disagree about what a
// third catalog should receive -- so it's surfaced for a manual pick
// instead of guessed at.
struct CrossSourceSyncConflict
{
    Track target;  // the shared target track, currently missing/behind on both proposals

    Track sourceA;
    std::vector<CuePoint> cuesFromA;
    // A 0:00 memory cue is almost always accidental (see
    // domain::JunkCueFinder's own doc comment) -- flagged here as a hint
    // toward resolving the conflict, e.g. by cleaning that cue up first
    // and re-syncing, rather than a reason to silently drop it: it's not
    // itself proof the two sides agree once removed, just something
    // worth knowing before making the manual choice.
    bool sourceAHasJunkCue = false;

    Track sourceB;
    std::vector<CuePoint> cuesFromB;
    bool sourceBHasJunkCue = false;
};

struct CrossSourceConflictSplit
{
    std::vector<SyncPlan> nonConflicting;            // safe to apply directly, unchanged
    std::vector<CrossSourceSyncConflict> conflicts;  // need a manual pick first
};

class CrossSourceConflictDetector
{
public:
    // actionablePlans: every actionable plan (direction != None) across
    // every pair computed for one stick, already combined into one list
    // (see gui::SyncController::analyze()). Groups plans by (target
    // format, target track's exact file path) -- see
    // domain::matchTracks()'s own doc comment for why file path is this
    // codebase's most reliable cross-catalog track identity signal, and
    // therefore the right key for "is this actually the same target
    // track" here too.
    //
    // A group of size 1 passes through untouched. A group of size 2
    // whose two proposals agree (domain::cueSetsEqual -- the same
    // order/comment/color-tolerant comparison SyncPlanner itself already
    // uses to avoid manufacturing a mismatch that could never resolve)
    // collapses to a single plan, since either proposal produces the
    // same result. A group of size 2 that genuinely disagrees becomes a
    // CrossSourceSyncConflict instead, and is not present in
    // nonConflicting at all until a caller resolves it.
    //
    // A group larger than 2 isn't possible with today's three-catalog
    // ceiling (a target format can only receive proposals from the other
    // two), so it isn't modeled -- TODO if a fourth catalog is ever
    // added, this needs real N-way resolution, not just a pairwise
    // comparison.
    static CrossSourceConflictSplit detect(const std::vector<SyncPlan> &actionablePlans);
};

}  // namespace seabass::domain
