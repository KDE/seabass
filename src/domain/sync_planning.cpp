// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/sync_planning.hpp"

#include "domain/engine_cue_translation.hpp"
#include "domain/junk_cue.hpp"
#include "domain/track_matching.hpp"

#include <algorithm>
#include <optional>

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

// A pair with exactly one Engine side. Engine holds no memory cues, so
// the other side ("X") is compared with Engine in X's own terms: an
// Engine hot cue or saved loop sitting where X has a memory cue is that
// memory cue's translation (cuesFromEngine), not a hot cue of Engine's
// own, and what a write onto Engine stores is X's cues as Engine holds
// them (translateCuesForEngine). Directions and the choice rule are the
// generic planner's; only the two views differ.
//
// Memory cues agree when every one of X's that fits on a pad is on one,
// and Engine's main cue is among X's memory cues (or neither has one).
// When X lacks Engine's main cue AND Engine lacks a translation, X is
// written first and Engine on the next sync: a plan has one direction.
SyncPlan planWithEngine(const SyncMatch &original, const SyncMatch &match, bool aIsEngine,
                        std::chrono::system_clock::time_point mtimeA, std::chrono::system_clock::time_point mtimeB)
{
    const Track &x = aIsEngine ? match.trackB : match.trackA;
    const Track &e = aIsEngine ? match.trackA : match.trackB;
    const SyncPlan::Direction toX = aIsEngine ? SyncPlan::Direction::ToB : SyncPlan::Direction::ToA;
    const SyncPlan::Direction toE = aIsEngine ? SyncPlan::Direction::ToA : SyncPlan::Direction::ToB;

    SyncPlan result;
    result.match = original;

    const std::vector<CuePoint> hotX = cuesOfKind(x.cues, CuePoint::Kind::Hot);
    const std::vector<CuePoint> memoryX = cuesOfKind(x.cues, CuePoint::Kind::Memory);
    const EngineCueTranslation translation = translateCuesForEngine(x.cues, e.cues);
    const CuesFromEngine seen = cuesFromEngine(e.cues, memoryX);
    result.cuesLeftOut = translation.leftOut;

    if (x.cues.empty() && e.cues.empty()) {
        result.kind = SyncPlan::Kind::NoCues;
        return result;
    }

    // What a write onto X carries: Engine's own hot cues and loops, and
    // every memory cue either side has (X's own, Engine's translations of
    // them, and Engine's main cue), with X's colours kept.
    const auto ontoX = [&](const std::vector<CuePoint> &hotFromEngine) {
        std::vector<CuePoint> cues = hotFromEngine;
        for (const CuePoint &cue : unionByPosition(memoryX, seen.memoryCues)) {
            cues.push_back(cue);
        }
        return keepExistingColours(std::move(cues), x.cues);
    };
    // What a write onto Engine carries: X's cues as Engine holds them.
    // With a hot-cue choice this is the X-wins set, Engine's own main
    // cue included (translateCuesForEngine keeps it).
    const auto ontoE = [&]() { return keepExistingColours(translation.cues, e.cues); };

    if (e.cues.empty()) {
        if (translation.cues.empty()) {
            // Only cues Engine cannot hold: nothing to write.
            result.kind = SyncPlan::Kind::AlreadyConsistent;
            return result;
        }
        result.kind = aIsEngine ? SyncPlan::Kind::BOnly : SyncPlan::Kind::AOnly;
        result.direction = toE;
        result.cuesToApply = ontoE();
        return result;
    }
    if (x.cues.empty()) {
        result.kind = aIsEngine ? SyncPlan::Kind::AOnly : SyncPlan::Kind::BOnly;
        result.direction = toX;
        result.cuesToApply = ontoX(seen.hotCues);
        return result;
    }

    // X's memory cues that fit on Engine, and whether Engine has them.
    std::vector<CuePoint> memoryXHeld;
    for (const CuePoint &cue : memoryX) {
        const bool leftOut = std::any_of(translation.leftOut.begin(), translation.leftOut.end(),
                                         [&](const CuePoint &out) { return samePosition(out, cue) && out.isLoop == cue.isLoop; });
        if (!leftOut) {
            memoryXHeld.push_back(cue);
        }
    }
    std::optional<CuePoint> mainE;
    for (const CuePoint &cue : e.cues) {
        if (cue.kind == CuePoint::Kind::Memory) {
            mainE = cue;
        }
    }
    // Engine holds a memory cue as a pad or as its main cue; either is
    // that cue at that place (seen.memoryCues has both).
    const bool engineHoldsMemory = positionsCoveredBy(memoryXHeld, seen.memoryCues);
    bool xLacksMain = false;
    if (mainE) {
        xLacksMain = std::none_of(memoryX.begin(), memoryX.end(),
                                  [&](const CuePoint &cue) { return !cue.isLoop && samePosition(cue, *mainE); });
    }
    const bool memoryAgrees = engineHoldsMemory && !xLacksMain;
    const bool hotAgrees = cueSetsEqual(hotX, seen.hotCues);

    if (hotAgrees && memoryAgrees) {
        result.kind = SyncPlan::Kind::AlreadyConsistent;
        return result;
    }
    result.kind = SyncPlan::Kind::Conflict;

    const bool perTrack = match.trackA.metadataModifiedAt > 0 && match.trackB.metadataModifiedAt > 0;
    const bool aIsNewer = perTrack ? match.trackA.metadataModifiedAt > match.trackB.metadataModifiedAt
                                   : mtimeA > mtimeB;
    const bool xIsNewer = aIsEngine ? !aIsNewer : aIsNewer;

    if (hotAgrees) {
        // Only memory cues differ. X lacks Engine's main cue: X is written,
        // it can hold every memory cue. Else Engine lacks a translation or
        // its main cue: Engine is written.
        result.direction = xLacksMain ? toX : toE;
        result.cuesToApply = xLacksMain ? ontoX(seen.hotCues) : ontoE();
        return result;
    }

    // Hot cues differ. Only one side has any: it wins, nothing is at risk.
    if (hotX.empty() != seen.hotCues.empty()) {
        const bool xHasHot = !hotX.empty();
        result.direction = xHasHot ? toE : toX;
        result.cuesToApply = xHasHot ? ontoE() : ontoX(seen.hotCues);
        return result;
    }

    // Both have hot cues of their own and they differ: the DJ chooses.
    result.hotCuesNeedChoice = true;
    std::vector<CuePoint> ifXWins = ontoE();
    std::vector<CuePoint> ifEWins = ontoX(seen.hotCues);
    result.cuesIfAWins = aIsEngine ? ifEWins : ifXWins;
    result.cuesIfBWins = aIsEngine ? ifXWins : ifEWins;
    result.direction = xIsNewer ? toE : toX;
    result.cuesToApply = xIsNewer ? ifXWins : ifEWins;
    return result;
}

}  // namespace

SyncPlan SyncPlanner::plan(const SyncMatch &original, std::chrono::system_clock::time_point mtimeA,
                            std::chrono::system_clock::time_point mtimeB)
{
    // Preferences -> Music -> "Ignore cues at 0:00": a cue the preference
    // ignores is not part of a sync. It is not compared, so a track whose
    // only difference is such a cue is consistent; it is not copied; and
    // because every write replaces a track's whole cue set, it is not kept
    // when the other side's cues are written over the track that had it.
    // Library Health's Stray Cues page is where those are cleaned off. With
    // the preference off this strips only a negative position, a format's
    // "no cue" sentinel (see domain::isJunkCue).
    //
    // The plan carries the tracks as scanned, those cues included, so the
    // page can show each side as it is and say what a write drops.
    SyncMatch match = original;
    match.trackA.cues = withoutJunkCues(original.trackA.cues);
    match.trackB.cues = withoutJunkCues(original.trackB.cues);

    const bool aIsEngine = match.trackA.format == "engine";
    const bool bIsEngine = match.trackB.format == "engine";
    if (aIsEngine != bIsEngine) {
        return planWithEngine(original, match, aIsEngine, mtimeA, mtimeB);
    }

    SyncPlan result;
    result.match = original;

    bool aHasCues = !match.trackA.cues.empty();
    bool bHasCues = !match.trackB.cues.empty();

    if (!aHasCues && !bHasCues) {
        result.kind = SyncPlan::Kind::NoCues;
        return result;
    }

    if (aHasCues && !bHasCues) {
        result.kind = SyncPlan::Kind::AOnly;
        result.direction = SyncPlan::Direction::ToB;
        result.cuesToApply = keepExistingColours(match.trackA.cues, match.trackB.cues);
        return result;
    }

    if (!aHasCues && bHasCues) {
        result.kind = SyncPlan::Kind::BOnly;
        result.direction = SyncPlan::Direction::ToA;
        result.cuesToApply = keepExistingColours(match.trackB.cues, match.trackA.cues);
        return result;
    }

    // Both sides have cues, and both hold hot cues and memory cues alike.
    // Decided apart all the same: a hot cue is a pad, a memory cue a place.
    const std::vector<CuePoint> hotA = cuesOfKind(match.trackA.cues, CuePoint::Kind::Hot);
    const std::vector<CuePoint> hotB = cuesOfKind(match.trackB.cues, CuePoint::Kind::Hot);
    const std::vector<CuePoint> memoryA = cuesOfKind(match.trackA.cues, CuePoint::Kind::Memory);
    const std::vector<CuePoint> memoryB = cuesOfKind(match.trackB.cues, CuePoint::Kind::Memory);

    if (cueSetsEqual(hotA, hotB) && cueSetsEqual(memoryA, memoryB)) {
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

    // Hot cues agree, only memory cues differ: the older side takes the
    // union, so no memory cue is lost from either.
    if (cueSetsEqual(hotA, hotB)) {
        const bool writeA = !aIsNewer;
        const Track &target = writeA ? match.trackA : match.trackB;
        result.direction = writeA ? SyncPlan::Direction::ToA : SyncPlan::Direction::ToB;
        result.cuesToApply = cuesOfKind(target.cues, CuePoint::Kind::Hot);
        for (const CuePoint &cue : unionByPosition(memoryA, memoryB)) {
            result.cuesToApply.push_back(cue);
        }
        result.cuesToApply = keepExistingColours(std::move(result.cuesToApply), target.cues);
        return result;
    }

    // Hot cues differ. What each side would write onto the other: its own
    // hot cues, plus the union of memory cues. Whatever the target already
    // knew about these cues' colours stays: a format that has none for a
    // kind is silent rather than grey, and silence is not an instruction
    // to erase.
    const auto writeOnto = [&](const Track &from, const Track &onto) {
        std::vector<CuePoint> cues = cuesOfKind(from.cues, CuePoint::Kind::Hot);
        for (const CuePoint &cue : unionByPosition(memoryA, memoryB)) {
            cues.push_back(cue);
        }
        return keepExistingColours(std::move(cues), onto.cues);
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
