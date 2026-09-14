// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include "domain/cross_source_sync_conflict.hpp"

using namespace seabass::domain;

namespace
{

Track makeTrack(const std::string &format, const std::string &sourceId, const std::string &filePath,
                 std::vector<CuePoint> cues)
{
    Track t;
    t.format = format;
    t.sourceId = sourceId;
    t.filePath = filePath;
    t.title = "Song";
    t.cues = std::move(cues);
    return t;
}

// One pairwise plan: sourceTrack -> targetTrack, ToB direction (matches
// how runApplyTask/SyncController always read source/target off a plan).
SyncPlan makePlan(Track sourceTrack, Track targetTrack, std::vector<CuePoint> cuesToApply)
{
    SyncPlan plan;
    plan.kind = SyncPlan::Kind::AOnly;
    plan.match.trackA = std::move(sourceTrack);
    plan.match.trackB = std::move(targetTrack);
    plan.direction = SyncPlan::Direction::ToB;
    plan.cuesToApply = std::move(cuesToApply);
    return plan;
}

CuePoint hot(int number, double positionMs)
{
    CuePoint c;
    c.kind = CuePoint::Kind::Hot;
    c.hotCueNumber = number;
    c.positionMs = positionMs;
    return c;
}

CuePoint memory(double positionMs)
{
    CuePoint c;
    c.kind = CuePoint::Kind::Memory;
    c.positionMs = positionMs;
    return c;
}

}  // namespace

int main()
{
    const std::string targetPath = "/stick/Contents/Song.mp3";

    // Case a: two pairs propose the same cues to the same OneLibrary
    // target, just in a different order -- collapses to one
    // nonConflicting plan, no conflict reported.
    {
        Track oneLibA = makeTrack("onelibrary", "ol1", targetPath, {});
        Track oneLibB = makeTrack("onelibrary", "ol1", targetPath, {});
        Track rb = makeTrack("rekordbox", "rb1", targetPath, {});
        Track engine = makeTrack("engine", "en1", targetPath, {});

        std::vector<CuePoint> rbOrder = {hot(2, 5000.0), hot(1, 1000.0)};
        std::vector<CuePoint> engineOrder = {hot(1, 1000.0), hot(2, 5000.0)};

        auto planFromRb = makePlan(rb, oneLibA, rbOrder);
        auto planFromEngine = makePlan(engine, oneLibB, engineOrder);

        auto split = CrossSourceConflictDetector::detect({planFromRb, planFromEngine});
        assert(split.nonConflicting.size() == 1);
        assert(split.conflicts.empty());
        std::cout << "case a (identical cues, different order, collapses with no conflict) OK\n";
    }

    // Case b: two pairs genuinely disagree about what OneLibrary should
    // receive -- becomes a conflict, absent from nonConflicting, with
    // correct source/cue assignment on both sides.
    {
        Track oneLibA = makeTrack("onelibrary", "ol1", targetPath, {});
        Track oneLibB = makeTrack("onelibrary", "ol1", targetPath, {});
        Track rb = makeTrack("rekordbox", "rb1", targetPath, {});
        Track engine = makeTrack("engine", "en1", targetPath, {});

        std::vector<CuePoint> rbCues = {hot(1, 1000.0)};
        std::vector<CuePoint> engineCues = {hot(1, 1000.0), hot(2, 9000.0)};

        auto planFromRb = makePlan(rb, oneLibA, rbCues);
        auto planFromEngine = makePlan(engine, oneLibB, engineCues);

        auto split = CrossSourceConflictDetector::detect({planFromRb, planFromEngine});
        assert(split.nonConflicting.empty());
        assert(split.conflicts.size() == 1);
        const auto &conflict = split.conflicts[0];
        assert(conflict.target.format == "onelibrary");
        assert(conflict.sourceA.format == "rekordbox");
        assert(conflict.cuesFromA.size() == 1);
        assert(conflict.sourceB.format == "engine");
        assert(conflict.cuesFromB.size() == 2);
        assert(!conflict.sourceAHasJunkCue);
        assert(!conflict.sourceBHasJunkCue);
        std::cout << "case b (genuine disagreement becomes a conflict, correct assignment) OK\n";
    }

    // Case c: a target appearing in only one pair's actionable plans
    // passes through untouched.
    {
        Track oneLib = makeTrack("onelibrary", "ol1", targetPath, {});
        Track rb = makeTrack("rekordbox", "rb1", targetPath, {});
        auto planFromRb = makePlan(rb, oneLib, {hot(1, 1000.0)});

        auto split = CrossSourceConflictDetector::detect({planFromRb});
        assert(split.nonConflicting.size() == 1);
        assert(split.conflicts.empty());
        std::cout << "case c (single-source target passes through untouched) OK\n";
    }

    // Case d: the disagreement is solely because one side carries an
    // extra 0:00 memory (junk) cue. Still a real conflict -- flagging is
    // a hint for the user, not a silent resolution.
    {
        Track oneLibA = makeTrack("onelibrary", "ol1", targetPath, {});
        Track oneLibB = makeTrack("onelibrary", "ol1", targetPath, {});
        Track rb = makeTrack("rekordbox", "rb1", targetPath, {});
        Track engine = makeTrack("engine", "en1", targetPath, {});

        std::vector<CuePoint> rbCues = {hot(1, 1000.0)};
        std::vector<CuePoint> engineCues = {hot(1, 1000.0), memory(0.0)};

        auto planFromRb = makePlan(rb, oneLibA, rbCues);
        auto planFromEngine = makePlan(engine, oneLibB, engineCues);

        auto split = CrossSourceConflictDetector::detect({planFromRb, planFromEngine});
        assert(split.conflicts.size() == 1);
        const auto &conflict = split.conflicts[0];
        assert(!conflict.sourceAHasJunkCue);
        assert(conflict.sourceBHasJunkCue);
        std::cout << "case d (junk 0:00 cue flagged as a hint, conflict still reported, not silently resolved) OK\n";
    }

    std::cout << "All cross_source_sync_conflict_test cases passed.\n";
    // A hot cue conflict between one pair's own two sides is taken out of the
    // plans as a choice, and never left behind to be applied on a clock.
    {
        seabass::domain::SyncPlan choice;
        choice.kind = seabass::domain::SyncPlan::Kind::Conflict;
        choice.hotCuesNeedChoice = true;
        choice.match.trackA.format = "rekordbox";
        choice.match.trackA.sourceId = "r1";
        choice.match.trackB.format = "engine";
        choice.match.trackB.sourceId = "e1";
        choice.direction = seabass::domain::SyncPlan::Direction::ToA;  // the suggestion only
        choice.cuesIfAWins = {seabass::domain::CuePoint{seabass::domain::CuePoint::Kind::Hot, 1, 1000.0, "", ""}};
        choice.cuesIfBWins = {seabass::domain::CuePoint{seabass::domain::CuePoint::Kind::Hot, 1, 5000.0, "", ""}};

        seabass::domain::SyncPlan plain;
        plain.kind = seabass::domain::SyncPlan::Kind::AOnly;
        plain.direction = seabass::domain::SyncPlan::Direction::ToB;
        plain.match.trackB.sourceId = "e2";

        std::vector<seabass::domain::SyncPlan> plans = {choice, plain};
        const auto choices = seabass::domain::CrossSourceConflictDetector::takeHotCueChoices(plans);
        assert(plans.size() == 1 && plans[0].match.trackB.sourceId == "e2" && "only the plain plan stays appliable");
        assert(choices.size() == 1);
        assert(choices[0].samePair);
        assert(choices[0].sourceA.sourceId == "r1" && choices[0].sourceB.sourceId == "e1");
        assert(choices[0].cuesFromA.front().positionMs == 1000.0 && "choosing A writes A's hot cues onto B");
        assert(choices[0].cuesFromB.front().positionMs == 5000.0 && "choosing B writes B's hot cues onto A");
        std::cout << "case hot-cue-choice (a same-pair hot cue conflict becomes a choice, not a plan) OK\n";
    }

    return 0;
}
