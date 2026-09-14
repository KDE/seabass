// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/sync_planning.hpp"

#include "domain/track_matching.hpp"

namespace seabass::domain
{

std::vector<SyncMatch> TrackMatcher::match(const std::vector<Track> &tracksA, const std::vector<Track> &tracksB)
{
    std::vector<SyncMatch> matches;
    for (const auto &[trackA, trackB] : matchTracks(tracksA, tracksB)) {
        matches.push_back({*trackA, *trackB});
    }
    return matches;
}

namespace
{

std::vector<CuePoint> cuesOfKind(const std::vector<CuePoint> &cues, CuePoint::Kind kind)
{
    std::vector<CuePoint> out;
    for (const CuePoint &cue : cues) {
        if (cue.kind == kind) {
            out.push_back(cue);
        }
    }
    return out;
}

// The tolerance cueSetsEqual() allows, so a memory cue that drifted by a
// cross-format rounding is still the same cue here.
constexpr double MemoryCuePositionToleranceMs = 1000.0;

bool samePosition(const CuePoint &a, const CuePoint &b)
{
    double distance = a.positionMs - b.positionMs;
    if (distance < 0) {
        distance = -distance;
    }
    return distance <= MemoryCuePositionToleranceMs;
}

// True when every cue in `sub` has one at the same position in `super`,
// and `sub` holds one whenever `super` holds any.
//
// Not vacuous for an empty `sub`: an Engine track with no memory cue at all,
// against a rekordbox track with three, is Engine missing the one it could
// hold, which a sync should put there -- not agreement.
bool positionsCoveredBy(const std::vector<CuePoint> &sub, const std::vector<CuePoint> &super)
{
    if (sub.empty()) {
        return super.empty();
    }
    for (const CuePoint &cue : sub) {
        bool found = false;
        for (const CuePoint &other : super) {
            if (samePosition(cue, other)) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

// Every cue in `a`, then every cue in `b` not already at one of those
// positions.
std::vector<CuePoint> unionByPosition(const std::vector<CuePoint> &a, const std::vector<CuePoint> &b)
{
    std::vector<CuePoint> out = a;
    for (const CuePoint &cue : b) {
        bool present = false;
        for (const CuePoint &existing : out) {
            if (samePosition(cue, existing)) {
                present = true;
                break;
            }
        }
        if (!present) {
            out.push_back(cue);
        }
    }
    return out;
}

}  // namespace

SyncPlan SyncPlanner::plan(const SyncMatch &match, std::chrono::system_clock::time_point mtimeA,
                            std::chrono::system_clock::time_point mtimeB)
{
    SyncPlan result;
    result.match = match;

    bool aHasCues = !match.trackA.cues.empty();
    bool bHasCues = !match.trackB.cues.empty();

    if (!aHasCues && !bHasCues) {
        result.kind = SyncPlan::Kind::NoCues;
        return result;
    }

    if (aHasCues && !bHasCues) {
        result.kind = SyncPlan::Kind::AOnly;
        result.direction = SyncPlan::Direction::ToB;
        result.cuesToApply = match.trackA.cues;
        return result;
    }

    if (!aHasCues && bHasCues) {
        result.kind = SyncPlan::Kind::BOnly;
        result.direction = SyncPlan::Direction::ToA;
        result.cuesToApply = match.trackB.cues;
        return result;
    }

    // Both sides have cues. Hot cues and memory cues are decided apart,
    // because they are not the same kind of fact on both formats: a hot
    // cue slot exists on every format, while Engine holds exactly one
    // memory cue and rekordbox holds as many as the DJ set. Comparing the
    // two lists whole made the difference in memory-cue CAPACITY look like
    // a disagreement, and resolving it by last-write-wins then replaced
    // rekordbox's memory cues with Engine's one -- after every sync,
    // because a sync is exactly what makes m.db the newer file.
    const std::vector<CuePoint> hotA = cuesOfKind(match.trackA.cues, CuePoint::Kind::Hot);
    const std::vector<CuePoint> hotB = cuesOfKind(match.trackB.cues, CuePoint::Kind::Hot);
    const std::vector<CuePoint> memoryA = cuesOfKind(match.trackA.cues, CuePoint::Kind::Memory);
    const std::vector<CuePoint> memoryB = cuesOfKind(match.trackB.cues, CuePoint::Kind::Memory);

    // Memory cues agree when the lists match, or when one side is Engine
    // and holds nothing the other lacks: Engine can only ever keep one, so
    // "its one is among rekordbox's three" is agreement, not a difference.
    const bool memoryAgrees = cueSetsEqual(memoryA, memoryB)
        || (match.trackA.format == "engine" && positionsCoveredBy(memoryA, memoryB))
        || (match.trackB.format == "engine" && positionsCoveredBy(memoryB, memoryA));

    if (cueSetsEqual(hotA, hotB) && memoryAgrees) {
        result.kind = SyncPlan::Kind::AlreadyConsistent;
        return result;
    }

    result.kind = SyncPlan::Kind::Conflict;

    // The clock. Each track's own edit time when both catalogs can say --
    // Engine records one per track, a rekordbox track's is its ANLZ file's
    // mtime -- because the whole-catalog mtime moves for every track the
    // moment any one is edited, so it cannot tell which side of THIS track
    // is newer. Falls back to the catalog mtimes only when either side
    // cannot date its own track (0).
    const bool perTrack = match.trackA.metadataModifiedAt > 0 && match.trackB.metadataModifiedAt > 0;
    const bool aIsNewer = perTrack
        ? match.trackA.metadataModifiedAt > match.trackB.metadataModifiedAt
        : mtimeA > mtimeB;
    const bool aIsEngine = match.trackA.format == "engine";
    const bool bIsEngine = match.trackB.format == "engine";

    // Memory cues are never resolved by overwriting Engine. Engine holds
    // one memory cue and its writer keeps the earliest it is given, so
    // sending it the union of both sides would replace its one cue with
    // the other side's earliest -- and when that cue exists nowhere else,
    // it is gone from both formats, with the next sync then calling the
    // track consistent because Engine's new one is among the other side's.
    //
    // Hot cues agree, only memory cues differ: write the side that is
    // actually missing something, and give it the union.
    //
    // With exactly one side Engine there are two ways to disagree. The
    // other side lacks a memory cue Engine has: write the other side, which
    // can hold them all. Or Engine has no memory cue at all while the other
    // side has some: write Engine -- it has nothing to lose, and its writer
    // keeps the earliest of the union, after which Engine's one cue is
    // among the other side's and the pair agrees. Writing the non-Engine
    // side there instead gave it back exactly what it had, left Engine
    // empty, and repeated -- backup, mirror write, conflict -- on every
    // sync. With neither or both sides Engine, the clock picks.
    if (cueSetsEqual(hotA, hotB)) {
        const auto engineIsEmpty = [](bool isEngine, const std::vector<CuePoint> &own,
                                      const std::vector<CuePoint> &other) {
            return isEngine && own.empty() && !other.empty();
        };
        bool writeA;
        if (aIsEngine != bIsEngine) {
            const bool engineIsA = aIsEngine;
            const bool writeEngine = engineIsA ? engineIsEmpty(true, memoryA, memoryB)
                                               : engineIsEmpty(true, memoryB, memoryA);
            writeA = writeEngine ? engineIsA : !engineIsA;
        } else {
            writeA = !aIsNewer;
        }
        const Track &target = writeA ? match.trackA : match.trackB;
        result.direction = writeA ? SyncPlan::Direction::ToA : SyncPlan::Direction::ToB;
        result.cuesToApply = cuesOfKind(target.cues, CuePoint::Kind::Hot);
        for (const CuePoint &cue : unionByPosition(memoryA, memoryB)) {
            result.cuesToApply.push_back(cue);
        }
        return result;
    }

    // Hot cues differ. What each side would write onto the other: its own
    // hot cues, plus memory cues by the rule above -- an Engine target that
    // already has a memory cue keeps its own (the other side's extras are
    // still on the other side, and the next sync, hot cues then agreeing,
    // carries Engine's across); any other target gets the union.
    const auto writeOnto = [&](const Track &from, const Track &onto) {
        std::vector<CuePoint> cues = cuesOfKind(from.cues, CuePoint::Kind::Hot);
        const std::vector<CuePoint> ontoMemory = cuesOfKind(onto.cues, CuePoint::Kind::Memory);
        const std::vector<CuePoint> memory =
            onto.format == "engine" && !ontoMemory.empty() ? ontoMemory : unionByPosition(memoryA, memoryB);
        cues.insert(cues.end(), memory.begin(), memory.end());
        return cues;
    };

    // Only one side has hot cues at all: nothing is at risk, so that side
    // wins whatever the clocks say. Writing the other way would erase hot
    // cues to make room for none.
    if (hotA.empty() != hotB.empty()) {
        const bool aHasHot = !hotA.empty();
        result.direction = aHasHot ? SyncPlan::Direction::ToB : SyncPlan::Direction::ToA;
        result.cuesToApply = aHasHot ? writeOnto(match.trackA, match.trackB) : writeOnto(match.trackB, match.trackA);
        return result;
    }

    // Both sides have hot cues and they differ: the DJ chooses. The newer
    // side is only the suggestion -- see SyncPlan::hotCuesNeedChoice for why
    // no clock here can settle it.
    result.hotCuesNeedChoice = true;
    result.cuesIfAWins = writeOnto(match.trackA, match.trackB);
    result.cuesIfBWins = writeOnto(match.trackB, match.trackA);
    result.direction = aIsNewer ? SyncPlan::Direction::ToB : SyncPlan::Direction::ToA;
    result.cuesToApply = aIsNewer ? result.cuesIfAWins : result.cuesIfBWins;
    return result;
}

namespace
{

bool sameCue(const CuePoint &a, const CuePoint &b)
{
    return a.kind == b.kind && samePosition(a, b)
        && (a.kind != CuePoint::Kind::Hot || a.hotCueNumber == b.hotCueNumber);
}

bool containsCue(const std::vector<CuePoint> &cues, const CuePoint &cue)
{
    for (const CuePoint &other : cues) {
        if (sameCue(cue, other)) {
            return true;
        }
    }
    return false;
}

}  // namespace

CueChange describeCueChange(const std::vector<CuePoint> &current, const std::vector<CuePoint> &written)
{
    CueChange change;
    for (const CuePoint &cue : written) {
        const bool hot = cue.kind == CuePoint::Kind::Hot;
        if (containsCue(current, cue)) {
            (hot ? change.keptHot : change.keptMemory)++;
        } else {
            (hot ? change.gainedHot : change.gainedMemory)++;
        }
    }
    for (const CuePoint &cue : current) {
        if (!containsCue(written, cue)) {
            (cue.kind == CuePoint::Kind::Hot ? change.droppedHot : change.droppedMemory)++;
        }
    }
    return change;
}

}  // namespace seabass::domain
