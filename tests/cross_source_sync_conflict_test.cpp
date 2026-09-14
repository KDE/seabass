// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <string>
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

    // The fourth review's finding: on a three-format stick one Engine track
    // whose hot cues differ from rekordbox's raises a choice in BOTH pairs
    // (rekordbox<->Engine and Engine<->OneLibrary). Two picks for one track
    // could swap its hot cues, so it is one card -- the rekordbox one -- and
    // any other plan for that file waits until the choice is made.
    {
        using seabass::domain::CuePoint;
        using seabass::domain::SyncPlan;
        const std::string file = "/media/RV2/Contents/song.mp3";
        const auto track = [&](const std::string &format, const std::string &id, const std::string &path) {
            seabass::domain::Track t;
            t.format = format;
            t.sourceId = id;
            t.filePath = path;
            return t;
        };
        const auto choiceBetween = [&](const seabass::domain::Track &a, const seabass::domain::Track &b) {
            SyncPlan p;
            p.kind = SyncPlan::Kind::Conflict;
            p.hotCuesNeedChoice = true;
            p.match.trackA = a;
            p.match.trackB = b;
            p.direction = SyncPlan::Direction::ToB;
            p.cuesIfAWins = {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "", ""}};
            p.cuesIfBWins = {CuePoint{CuePoint::Kind::Hot, 1, 5000.0, "", ""}};
            return p;
        };
        // Pair order as the controller builds them: rekordbox<->Engine, then Engine<->OneLibrary.
        SyncPlan viaRekordbox = choiceBetween(track("rekordbox", "r1", file), track("engine", "e1", file));
        SyncPlan viaOneLibrary = choiceBetween(track("engine", "e1", file), track("onelibrary", "o1", file));
        // A memory-only plan for the same file, which Stage All must not
        // write before the choice is made.
        SyncPlan sameFileOther;
        sameFileOther.kind = SyncPlan::Kind::Conflict;
        sameFileOther.direction = SyncPlan::Direction::ToB;
        sameFileOther.match.trackA = track("engine", "e1", file);
        sameFileOther.match.trackB = track("onelibrary", "o1", file);
        // And one for another file entirely, which stays.
        SyncPlan otherFile;
        otherFile.kind = SyncPlan::Kind::AOnly;
        otherFile.direction = SyncPlan::Direction::ToB;
        otherFile.match.trackA = track("rekordbox", "r2", "/media/RV2/Contents/other.mp3");
        otherFile.match.trackB = track("engine", "e2", "/media/RV2/Contents/other.mp3");

        // OneLibrary's choice listed first, so "prefer rekordbox" is really exercised.
        std::vector<SyncPlan> plans = {viaOneLibrary, sameFileOther, viaRekordbox, otherFile};
        const auto choices = seabass::domain::CrossSourceConflictDetector::takeHotCueChoices(plans);
        assert(choices.size() == 1 && "one card for one track, not one per pair");
        const bool hasRekordbox = choices[0].sourceA.format == "rekordbox" || choices[0].sourceB.format == "rekordbox";
        assert(hasRekordbox && "the kept choice is the rekordbox one; its write mirrors into OneLibrary");
        assert(plans.size() == 1 && plans[0].match.trackA.sourceId == "r2"
               && "every other plan for the undecided file waits; other files are untouched");
        std::cout << "case hot-cue-choice-three-formats (one card per file, and nothing else writes it meanwhile) OK\n";

        // Without rekordbox on the stick the OneLibrary choice is the card.
        std::vector<SyncPlan> onlyOneLibrary = {viaOneLibrary};
        const auto alone = seabass::domain::CrossSourceConflictDetector::takeHotCueChoices(onlyOneLibrary);
        assert(alone.size() == 1 && onlyOneLibrary.empty());
        std::cout << "case hot-cue-choice-onelibrary-only (a OneLibrary choice stands when it is the only one) OK\n";
    }

    return 0;
}
