// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/sync_planning.hpp"

#include "domain/engine_cue_translation.hpp"
#include "domain/junk_cue.hpp"
#include "domain/track_matching.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>

namespace seabass::domain
{

std::vector<SyncMatch> TrackMatcher::match(const std::vector<Track> &tracksA, const std::vector<Track> &tracksB)
{
    std::vector<SyncMatch> matches;
    // Two catalogs of one stick: see MatchScope::OneStick.
    for (const auto &[trackA, trackB] : matchTracks(tracksA, tracksB, MatchScope::OneStick)) {
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

bool samePosition(const CuePoint &a, const CuePoint &b, double toleranceMs)
{
    return std::abs(a.positionMs - b.positionMs) < toleranceMs;
}

// The same memory cue: a cue against a cue or a loop against a loop, at
// the same place.
bool samePlace(const CuePoint &a, const CuePoint &b, double toleranceMs)
{
    return a.isLoop == b.isLoop && samePosition(a, b, toleranceMs);
}

// Every cue in `sub` has one at the same place in `super`. True for an
// empty `sub`.
bool allWithin(const std::vector<CuePoint> &sub, const std::vector<CuePoint> &super, double toleranceMs)
{
    return std::all_of(sub.begin(), sub.end(), [&](const CuePoint &cue) {
        return std::any_of(super.begin(), super.end(),
                           [&](const CuePoint &other) { return samePlace(cue, other, toleranceMs); });
    });
}

// Two lists of memory cues hold the same cues: as many of each kind, each
// at the same place. cueSetsEqual() does not tell a loop from a cue.
bool memorySetsEqual(const std::vector<CuePoint> &a, const std::vector<CuePoint> &b, double toleranceMs)
{
    if (a.size() != b.size()) {
        return false;
    }
    const auto sorted = [](std::vector<CuePoint> cues) {
        std::sort(cues.begin(), cues.end(), [](const CuePoint &x, const CuePoint &y) {
            return x.isLoop != y.isLoop ? !x.isLoop : x.positionMs < y.positionMs;
        });
        return cues;
    };
    const std::vector<CuePoint> x = sorted(a);
    const std::vector<CuePoint> y = sorted(b);
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!samePlace(x[i], y[i], toleranceMs)) {
            return false;
        }
    }
    return true;
}

// True when every cue in `sub` has one at the same position in `super`,
// and `sub` holds one whenever `super` holds any.
//
// Not vacuous for an empty `sub`: an Engine track with no memory cue at all,
// against a rekordbox track with three, is Engine missing the one it could
// hold, which a sync should put there -- not agreement.
bool positionsCoveredBy(const std::vector<CuePoint> &sub, const std::vector<CuePoint> &super, double toleranceMs)
{
    if (sub.empty()) {
        return super.empty();
    }
    return allWithin(sub, super, toleranceMs);
}

// Every cue in `a`, then every cue in `b` not already at one of those
// places.
std::vector<CuePoint> unionByPosition(const std::vector<CuePoint> &a, const std::vector<CuePoint> &b,
                                      double toleranceMs)
{
    std::vector<CuePoint> out = a;
    for (const CuePoint &cue : b) {
        const bool present = std::any_of(out.begin(), out.end(), [&](const CuePoint &existing) {
            return samePlace(cue, existing, toleranceMs);
        });
        if (!present) {
            out.push_back(cue);
        }
    }
    return out;
}

std::string formatTempo(double bpm)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", bpm);
    return text;
}

// "124" for a whole tempo, "127.5" otherwise.
std::string formatBeatTempo(double bpm)
{
    const double rounded = std::round(bpm);
    if (std::abs(bpm - rounded) < 0.05) {
        return std::to_string(static_cast<long long>(rounded));
    }
    return formatTempo(bpm);
}

std::string pluralPads(std::size_t count)
{
    return std::to_string(count) + (count == 1 ? " more pad differs" : " more pads differ");
}

// Beyond this two cues on one pad are plainly different cues rather than
// one cue apart: the measured stick had none between 53 ms and a second,
// and the 4 beyond a second were different cues.
constexpr double DifferentCueMs = 1000.0;

struct PadPair
{
    int pad = 0;
    const CuePoint *a = nullptr;
    const CuePoint *b = nullptr;
    // What the pad holds on each side at all: "empty" and "no loop" are
    // different things to say.
    bool aHasPad = false;
    bool bHasPad = false;
};

// How two hot cue sets differ, pad by pad. Equal when all three lists are
// empty: the same pads, each holding the same kinds (a cue, a loop or
// both, as Engine can), each within the tolerance. Colour and comment
// never count (see cueSetsEqual).
struct HotDifference
{
    std::vector<PadPair> differ;     // a cue or loop on one side only, or a second or more apart
    std::vector<PadPair> apart;      // on both, beyond the tolerance, under a second
    std::vector<PadPair> loopVsCue;  // a loop on one side and only a cue on the other

    bool any() const { return !differ.empty() || !apart.empty() || !loopVsCue.empty(); }
};

HotDifference compareHotCues(const std::vector<CuePoint> &hotA, const std::vector<CuePoint> &hotB,
                             double toleranceMs)
{
    // Per pad and side: its cues and its loops, each in time order.
    struct Pad
    {
        std::vector<const CuePoint *> cues;
        std::vector<const CuePoint *> loops;
        std::size_t count() const { return cues.size() + loops.size(); }
    };
    std::map<int, std::pair<Pad, Pad>> byPad;
    for (const CuePoint &cue : hotA) {
        Pad &pad = byPad[cue.hotCueNumber].first;
        (cue.isLoop ? pad.loops : pad.cues).push_back(&cue);
    }
    for (const CuePoint &cue : hotB) {
        Pad &pad = byPad[cue.hotCueNumber].second;
        (cue.isLoop ? pad.loops : pad.cues).push_back(&cue);
    }
    const auto byPosition = [](const CuePoint *x, const CuePoint *y) { return x->positionMs < y->positionMs; };
    HotDifference result;
    for (auto &[pad, sides] : byPad) {
        Pad &a = sides.first;
        Pad &b = sides.second;
        const bool aHas = a.count() > 0;
        const bool bHas = b.count() > 0;
        // A pad that is one thing on each side, a loop here and a cue there.
        if (a.count() == 1 && b.count() == 1 && a.loops.size() != b.loops.size()) {
            result.loopVsCue.push_back({pad, a.loops.empty() ? a.cues.front() : a.loops.front(),
                                        b.loops.empty() ? b.cues.front() : b.loops.front(), aHas, bHas});
            continue;
        }
        // Else cues against cues and loops against loops, in time order;
        // the first difference stands for the pad. One of each per pad is
        // the rule, but a list with more is compared in full.
        bool differs = false;
        for (const bool loops : {false, true}) {
            if (differs) {
                break;
            }
            std::vector<const CuePoint *> &x = loops ? a.loops : a.cues;
            std::vector<const CuePoint *> &y = loops ? b.loops : b.cues;
            std::sort(x.begin(), x.end(), byPosition);
            std::sort(y.begin(), y.end(), byPosition);
            for (std::size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
                const CuePoint *onA = i < x.size() ? x[i] : nullptr;
                const CuePoint *onB = i < y.size() ? y[i] : nullptr;
                const PadPair pair{pad, onA, onB, aHas, bHas};
                if (onA == nullptr || onB == nullptr) {
                    result.differ.push_back(pair);
                    differs = true;
                    break;
                }
                const double distance = std::abs(onA->positionMs - onB->positionMs);
                if (distance >= toleranceMs) {
                    (distance < DifferentCueMs ? result.apart : result.differ).push_back(pair);
                    differs = true;
                    break;
                }
            }
        }
    }
    return result;
}

// The reason for a choice between two sets of hot cues, A's and B's.
std::pair<SyncPlan::Reason, std::string> describeHotDifference(const HotDifference &difference,
                                                               const std::string &labelA,
                                                               const std::string &labelB,
                                                               const CueTolerance &tolerance)
{
    // Every pad that differs, however: the first reason's pad is named, the
    // rest counted.
    const std::size_t total = difference.apart.size() + difference.loopVsCue.size() + difference.differ.size();
    const auto position = [](const CuePoint &cue) {
        return (cue.isLoop ? "loop " : "") + formatCuePosition(cue.positionMs);
    };
    if (!difference.apart.empty()) {
        const PadPair &first = difference.apart.front();
        const long long distance = std::llround(std::abs(first.a->positionMs - first.b->positionMs));
        std::string text = "Pad " + std::to_string(first.pad) + (first.a->isLoop ? " loop" : "") + " is "
            + std::to_string(distance) + " ms apart";
        if (tolerance.fromTempo) {
            text += " (more than half a beat at " + formatBeatTempo(tolerance.bpm) + " BPM)";
        }
        text += ": " + labelA + " " + formatCuePosition(first.a->positionMs) + ", " + labelB + " "
            + formatCuePosition(first.b->positionMs);
        if (total > 1) {
            text += ", and " + pluralPads(total - 1);
        }
        return {SyncPlan::Reason::SamePadApart, text};
    }
    if (!difference.loopVsCue.empty()) {
        const PadPair &first = difference.loopVsCue.front();
        const std::string &loopSide = first.a->isLoop ? labelA : labelB;
        const std::string &cueSide = first.a->isLoop ? labelB : labelA;
        std::string text = "Pad " + std::to_string(first.pad) + " is a loop on " + loopSide + " and a cue on "
            + cueSide + " (";
        if (std::llround(first.a->positionMs) == std::llround(first.b->positionMs)) {
            text += formatCuePosition(first.a->positionMs) + ")";
        } else {
            text += labelA + " " + formatCuePosition(first.a->positionMs) + ", " + labelB + " "
                + formatCuePosition(first.b->positionMs) + ")";
        }
        if (total > 1) {
            text += ", and " + pluralPads(total - 1);
        }
        return {SyncPlan::Reason::LoopVsCue, text};
    }
    const auto side = [&](const std::string &label, const CuePoint *cue, bool hasPad, const CuePoint *other) {
        if (cue != nullptr) {
            return label + " " + position(*cue);
        }
        if (!hasPad) {
            return label + " empty";
        }
        return label + (other != nullptr && other->isLoop ? " no loop" : " no cue");
    };
    std::string text;
    const std::size_t shown = std::min<std::size_t>(difference.differ.size(), 3);
    for (std::size_t i = 0; i < shown; ++i) {
        const PadPair &pair = difference.differ[i];
        if (i > 0) {
            text += "; ";
        }
        text += "Pad " + std::to_string(pair.pad) + ": " + side(labelA, pair.a, pair.aHasPad, pair.b) + ", "
            + side(labelB, pair.b, pair.bHasPad, pair.a);
    }
    if (difference.differ.size() > shown) {
        text += "; and " + pluralPads(difference.differ.size() - shown);
    }
    return {SyncPlan::Reason::PadsDiffer, text};
}

std::string describeUncertainPads(const std::vector<CuesFromEngine::Uncertain> &uncertain, const std::string &label)
{
    if (uncertain.size() == 1) {
        return "Engine pad " + std::to_string(uncertain.front().pad.hotCueNumber) + " sits where " + label
            + " has a memory cue (" + formatCuePosition(uncertain.front().memory.positionMs)
            + "); a translated memory cue or a new hot cue?";
    }
    std::string pads;
    std::string places;
    const std::size_t shown = std::min<std::size_t>(uncertain.size(), 3);
    for (std::size_t i = 0; i < shown; ++i) {
        pads += (i == 0 ? "" : (i + 1 == shown && shown == uncertain.size() ? " and " : ", "))
            + std::to_string(uncertain[i].pad.hotCueNumber);
        places += (i == 0 ? "" : ", ") + formatCuePosition(uncertain[i].memory.positionMs);
    }
    if (uncertain.size() > shown) {
        pads += " and " + std::to_string(uncertain.size() - shown) + " more";
    }
    return "Engine pads " + pads + " sit where " + label + " has memory cues (" + places
        + "); translated memory cues or new hot cues?";
}

std::string describeTempoUnsure(const Track &a, const Track &b)
{
    const bool knownA = isKnownTempo(a.bpm);
    const bool knownB = isKnownTempo(b.bpm);
    std::string head;
    if (knownA && knownB) {
        head = "Tempos differ (" + formatTempo(a.bpm) + " vs " + formatTempo(b.bpm) + ")";
    } else if (knownA || knownB) {
        head = catalogDisplayName(knownA ? b.format : a.format) + " has no tempo for this track";
    } else {
        head = "Neither side has a tempo for this track";
    }
    return head + ", so cues within half a beat cannot be matched; "
        + std::to_string(static_cast<long long>(CueFallbackToleranceMs)) + " ms was used";
}

// Turns a conflict into a choice: nothing is applied until the DJ picks.
void makeChoice(SyncPlan &plan, SyncPlan::Reason reason, std::string text)
{
    plan.kind = SyncPlan::Kind::Conflict;
    plan.needsChoice = true;
    plan.direction = SyncPlan::Direction::None;
    plan.cuesToApply.clear();
    plan.reason = reason;
    plan.reasonText = std::move(text);
}

// A pair with exactly one Engine side. Engine holds no memory cues, so
// the other side ("X") is compared with Engine in X's own terms: an
// Engine hot cue or saved loop sitting where X has a memory cue is that
// memory cue's translation (cuesFromEngine), not a hot cue of Engine's
// own, and what a write onto Engine stores is X's cues as Engine holds
// them (translateCuesForEngine).
//
// Memory cues agree when every one of X's that fits on a pad is on one,
// and Engine's main cue is among X's memory cues (or neither has one).
// When X lacks Engine's main cue AND Engine lacks a translation, X is
// written first and Engine on the next sync: a plan has one direction.
SyncPlan planWithEngine(const SyncMatch &original, const SyncMatch &match, bool aIsEngine, double toleranceMs,
                        const CueTolerance &tolerance)
{
    const Track &x = aIsEngine ? match.trackB : match.trackA;
    const Track &e = aIsEngine ? match.trackA : match.trackB;
    const SyncPlan::Direction toX = aIsEngine ? SyncPlan::Direction::ToB : SyncPlan::Direction::ToA;
    const SyncPlan::Direction toE = aIsEngine ? SyncPlan::Direction::ToA : SyncPlan::Direction::ToB;

    SyncPlan result;
    result.match = original;

    const std::vector<CuePoint> hotX = cuesOfKind(x.cues, CuePoint::Kind::Hot);
    const std::vector<CuePoint> memoryX = cuesOfKind(x.cues, CuePoint::Kind::Memory);
    const EngineCueTranslation translation = translateCuesForEngine(x.cues, e.cues, toleranceMs);
    const CuesFromEngine seen = cuesFromEngine(e.cues, x.cues, toleranceMs);
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
        for (const CuePoint &cue : unionByPosition(memoryX, seen.memoryCues, toleranceMs)) {
            cues.push_back(cue);
        }
        return keepExistingColours(std::move(cues), x.cues, toleranceMs);
    };
    // What a write onto Engine carries: X's cues as Engine holds them,
    // Engine's own main cue included (translateCuesForEngine keeps it).
    const auto ontoE = [&]() { return keepExistingColours(translation.cues, e.cues, toleranceMs); };

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
        const bool leftOut = std::any_of(translation.leftOut.begin(), translation.leftOut.end(), [&](const CuePoint &out) {
            return samePosition(out, cue, toleranceMs) && out.isLoop == cue.isLoop;
        });
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
    const bool engineHoldsMemory = positionsCoveredBy(memoryXHeld, seen.memoryCues, toleranceMs);
    bool xLacksMain = false;
    if (mainE) {
        xLacksMain = std::none_of(memoryX.begin(), memoryX.end(), [&](const CuePoint &cue) {
            return !cue.isLoop && samePosition(cue, *mainE, toleranceMs);
        });
    }
    const bool memoryAgrees = engineHoldsMemory && !xLacksMain;
    const HotDifference hotDifference = compareHotCues(hotX, seen.hotCues, toleranceMs);

    if (!hotDifference.any() && memoryAgrees && seen.uncertain.empty()) {
        result.kind = SyncPlan::Kind::AlreadyConsistent;
        return result;
    }
    result.kind = SyncPlan::Kind::Conflict;
    // What each choice would write, for any conflict, so one that turns
    // into a choice later (SyncPlanner::plan's tempo check) has both ready.
    std::vector<CuePoint> ifXWins = ontoE();
    std::vector<CuePoint> ifEWins = ontoX(seen.hotCues);

    // An Engine pad at one of X's memory cues, on a pad Engine DJ's own
    // import would not have given it: a translation, or a hot cue the DJ
    // set on the player at that place? Nothing on the stick says which.
    if (!seen.uncertain.empty()) {
        // X's way: X's cues the way the import places them, so the pads in
        // question move to the translation's numbers (Engine's main cue is
        // kept). Engine's way: those pads are hot cues, and X gets them.
        std::vector<CuePoint> mainOnly;
        for (const CuePoint &cue : e.cues) {
            if (cue.kind != CuePoint::Kind::Hot) {
                mainOnly.push_back(cue);
            }
        }
        ifXWins = keepExistingColours(translateCuesForEngine(x.cues, mainOnly, toleranceMs).cues, e.cues, toleranceMs);
        std::vector<CuePoint> hotFromEngine = seen.hotCues;
        for (const auto &pad : seen.uncertain) {
            hotFromEngine.push_back(pad.pad);
        }
        ifEWins = ontoX(hotFromEngine);
        result.cuesIfAWins = aIsEngine ? ifEWins : ifXWins;
        result.cuesIfBWins = aIsEngine ? ifXWins : ifEWins;
        makeChoice(result, SyncPlan::Reason::EngineMemoryOrHotCue, describeUncertainPads(seen.uncertain, catalogDisplayName(x.format)));
        return result;
    }
    result.cuesIfAWins = aIsEngine ? ifEWins : ifXWins;
    result.cuesIfBWins = aIsEngine ? ifXWins : ifEWins;

    if (!hotDifference.any()) {
        // Only memory cues differ. X lacks Engine's main cue: X is written,
        // it can hold every memory cue. Else Engine lacks a translation or
        // its main cue: Engine is written. Either way the side written only
        // gains: no clock is asked.
        result.direction = xLacksMain ? toX : toE;
        result.cuesToApply = xLacksMain ? ifEWins : ifXWins;
        return result;
    }

    // Hot cues differ. Only one side has any: it wins, nothing is at risk.
    if (hotX.empty() != seen.hotCues.empty()) {
        const bool xHasHot = !hotX.empty();
        result.direction = xHasHot ? toE : toX;
        result.cuesToApply = xHasHot ? ifXWins : ifEWins;
        return result;
    }

    // Both have hot cues of their own and they differ: the DJ chooses. Said
    // in the pair's own order, A first, the way a page or the CLI lists it.
    const HotDifference inPairOrder = aIsEngine ? compareHotCues(seen.hotCues, hotX, toleranceMs) : hotDifference;
    auto [reason, text] = describeHotDifference(inPairOrder, catalogDisplayName(match.trackA.format),
                                                catalogDisplayName(match.trackB.format), tolerance);
    makeChoice(result, reason, std::move(text));
    return result;
}

SyncPlan planBetween(const SyncMatch &original, const SyncMatch &match, double toleranceMs,
                     const CueTolerance &tolerance)
{
    const bool aIsEngine = match.trackA.format == "engine";
    const bool bIsEngine = match.trackB.format == "engine";
    if (aIsEngine != bIsEngine) {
        return planWithEngine(original, match, aIsEngine, toleranceMs, tolerance);
    }

    SyncPlan result;
    result.match = original;

    const bool aHasCues = !match.trackA.cues.empty();
    const bool bHasCues = !match.trackB.cues.empty();

    if (!aHasCues && !bHasCues) {
        result.kind = SyncPlan::Kind::NoCues;
        return result;
    }

    if (aHasCues && !bHasCues) {
        result.kind = SyncPlan::Kind::AOnly;
        result.direction = SyncPlan::Direction::ToB;
        result.cuesToApply = keepExistingColours(match.trackA.cues, match.trackB.cues, toleranceMs);
        return result;
    }

    if (!aHasCues && bHasCues) {
        result.kind = SyncPlan::Kind::BOnly;
        result.direction = SyncPlan::Direction::ToA;
        result.cuesToApply = keepExistingColours(match.trackB.cues, match.trackA.cues, toleranceMs);
        return result;
    }

    // Both sides have cues, and both hold hot cues and memory cues alike.
    // Decided apart all the same: a hot cue is a pad, a memory cue a place.
    const std::vector<CuePoint> hotA = cuesOfKind(match.trackA.cues, CuePoint::Kind::Hot);
    const std::vector<CuePoint> hotB = cuesOfKind(match.trackB.cues, CuePoint::Kind::Hot);
    const std::vector<CuePoint> memoryA = cuesOfKind(match.trackA.cues, CuePoint::Kind::Memory);
    const std::vector<CuePoint> memoryB = cuesOfKind(match.trackB.cues, CuePoint::Kind::Memory);
    const HotDifference hotDifference = compareHotCues(hotA, hotB, toleranceMs);

    if (!hotDifference.any() && memorySetsEqual(memoryA, memoryB, toleranceMs)) {
        result.kind = SyncPlan::Kind::AlreadyConsistent;
        return result;
    }

    result.kind = SyncPlan::Kind::Conflict;

    // What each side would write onto the other: its own hot cues, plus
    // the union of memory cues. Whatever the target already knew about
    // these cues' colours stays: a format that has none for a kind is
    // silent rather than grey, and silence is not an instruction to erase.
    const auto writeOnto = [&](const Track &from, const Track &onto) {
        std::vector<CuePoint> cues = cuesOfKind(from.cues, CuePoint::Kind::Hot);
        for (const CuePoint &cue : unionByPosition(memoryA, memoryB, toleranceMs)) {
            cues.push_back(cue);
        }
        return keepExistingColours(std::move(cues), onto.cues, toleranceMs);
    };
    result.cuesIfAWins = writeOnto(match.trackA, match.trackB);
    result.cuesIfBWins = writeOnto(match.trackB, match.trackA);

    if (!hotDifference.any()) {
        // Hot cues agree, only memory cues differ. A side whose memory cues
        // are all among the other's only lacks some: it receives the
        // union and loses nothing. Both lacking something is two edits,
        // an addition on one side or a removal on the other, and only a
        // clock could say which came last; no clock on a stick can.
        // The side written keeps its own hot cues.
        const bool writeA = allWithin(memoryA, memoryB, toleranceMs);
        if (writeA || allWithin(memoryB, memoryA, toleranceMs)) {
            result.direction = writeA ? SyncPlan::Direction::ToA : SyncPlan::Direction::ToB;
            const Track &target = writeA ? match.trackA : match.trackB;
            result.cuesToApply = writeOnto(target, target);
            return result;
        }
        // Each side's own cues, not the union: the union is the same
        // either way, and what is being chosen is whose edit stands.
        result.cuesIfAWins = keepExistingColours(match.trackA.cues, match.trackB.cues, toleranceMs);
        result.cuesIfBWins = keepExistingColours(match.trackB.cues, match.trackA.cues, toleranceMs);
        makeChoice(result, SyncPlan::Reason::BothChanged,
                   "Changed on both sides; the stick's clocks cannot say which is newer");
        return result;
    }

    // Only one side has hot cues at all: nothing is at risk, so that side
    // wins. Writing the other way would erase hot cues to make room for
    // none.
    if (hotA.empty() != hotB.empty()) {
        const bool aHasHot = !hotA.empty();
        result.direction = aHasHot ? SyncPlan::Direction::ToB : SyncPlan::Direction::ToA;
        result.cuesToApply = aHasHot ? result.cuesIfAWins : result.cuesIfBWins;
        return result;
    }

    // Both sides have hot cues and they differ: the DJ chooses.
    auto [reason, text] = describeHotDifference(hotDifference, catalogDisplayName(match.trackA.format),
                                                catalogDisplayName(match.trackB.format), tolerance);
    makeChoice(result, reason, std::move(text));
    return result;
}

}  // namespace

std::string catalogDisplayName(const std::string &format)
{
    if (format == "engine") {
        return "Engine";
    }
    if (format == "onelibrary") {
        return "OneLibrary";
    }
    return format;
}

std::string formatCuePosition(double positionMs)
{
    const long long total = std::llround(std::max(0.0, positionMs));
    char text[48];
    std::snprintf(text, sizeof text, "%lld:%02lld.%03lld", total / 60000, (total / 1000) % 60, total % 1000);
    return text;
}

bool sameCuesForSync(const std::vector<CuePoint> &a, const std::vector<CuePoint> &b, double toleranceMs)
{
    return !compareHotCues(cuesOfKind(a, CuePoint::Kind::Hot), cuesOfKind(b, CuePoint::Kind::Hot), toleranceMs).any()
        && memorySetsEqual(cuesOfKind(a, CuePoint::Kind::Memory), cuesOfKind(b, CuePoint::Kind::Memory), toleranceMs);
}

std::string describeCuesLeftOut(const std::vector<CuePoint> &cuesLeftOut)
{
    if (cuesLeftOut.empty()) {
        return {};
    }
    std::string places;
    for (const CuePoint &cue : cuesLeftOut) {
        places += (places.empty() ? "" : ", ") + formatCuePosition(cue.positionMs);
    }
    const std::size_t count = cuesLeftOut.size();
    return std::to_string(count) + (count == 1 ? " cue stays" : " cues stay")
        + " off Engine: its eight pads are full (" + places + ")";
}

SyncPlan SyncPlanner::plan(const SyncMatch &original, std::chrono::system_clock::time_point /*mtimeA*/,
                            std::chrono::system_clock::time_point /*mtimeB*/)
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

    // Half a beat of the tempo both sides agree on; else the fallback.
    const CueTolerance tolerance = cueToleranceFor(match.trackA.bpm, match.trackB.bpm);
    SyncPlan result = planBetween(original, match, tolerance.ms, tolerance);
    result.positionToleranceMs = tolerance.ms;

    // Without an agreed tempo the fallback is a guess at what half a beat
    // would have been. Where half a beat of either side's tempo would have
    // decided otherwise, the DJ is asked, and told why.
    if (!tolerance.fromTempo && result.kind == SyncPlan::Kind::Conflict && tolerance.unsureUpToMs > tolerance.ms) {
        const SyncPlan wide = planBetween(original, match, tolerance.unsureUpToMs, tolerance);
        const bool decidedOtherwise = wide.kind != result.kind || wide.direction != result.direction
            || wide.needsChoice != result.needsChoice || wide.reason != result.reason;
        if (decidedOtherwise) {
            // A choice already had its own reason: kept in front, so the DJ
            // still sees which pad and how far.
            std::string text = describeTempoUnsure(match.trackA, match.trackB);
            if (!result.reasonText.empty()) {
                text = result.reasonText + ". " + text;
            }
            makeChoice(result, SyncPlan::Reason::TempoUnsure, std::move(text));
        }
    }
    return result;
}

namespace
{

bool sameCue(const CuePoint &a, const CuePoint &b, double toleranceMs)
{
    return a.kind == b.kind && samePosition(a, b, toleranceMs)
        && (a.kind != CuePoint::Kind::Hot || a.hotCueNumber == b.hotCueNumber);
}

bool containsCue(const std::vector<CuePoint> &cues, const CuePoint &cue, double toleranceMs)
{
    for (const CuePoint &other : cues) {
        if (sameCue(cue, other, toleranceMs)) {
            return true;
        }
    }
    return false;
}

}  // namespace

CueChange describeCueChange(const std::vector<CuePoint> &current, const std::vector<CuePoint> &written,
                            double toleranceMs)
{
    CueChange change;
    for (const CuePoint &cue : written) {
        const bool hot = cue.kind == CuePoint::Kind::Hot;
        if (containsCue(current, cue, toleranceMs)) {
            (hot ? change.keptHot : change.keptMemory)++;
        } else {
            (hot ? change.gainedHot : change.gainedMemory)++;
        }
    }
    for (const CuePoint &cue : current) {
        if (!containsCue(written, cue, toleranceMs)) {
            (cue.kind == CuePoint::Kind::Hot ? change.droppedHot : change.droppedMemory)++;
        }
    }
    return change;
}

}  // namespace seabass::domain
