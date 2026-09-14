// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <string>
#include <iostream>

#include "application/path_key.hpp"
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

    // Round 9's finding: two Engine targets with no file shared the key
    // {"engine", ""}, so a rekordbox plan onto one and a OneLibrary plan onto
    // the other read as one conflict -- and resolving it wrote one track's
    // cues onto the other. Rows without a file are never grouped.
    {
        auto rb = makeTrack("rekordbox", "r1", "/media/A/Contents/x.mp3", {hot(1, 1000.0)});
        auto ol = makeTrack("onelibrary", "o1", "/media/A/Contents/y.mp3", {hot(1, 9000.0)});
        auto engineX = makeTrack("engine", "eX", "", {});
        auto engineY = makeTrack("engine", "eY", "", {});
        auto split = CrossSourceConflictDetector::detect(
            {makePlan(rb, engineX, {hot(1, 1000.0)}), makePlan(ol, engineY, {hot(1, 9000.0)})});
        assert(split.conflicts.empty() && "two tracks without a file are not one conflict");
        assert(split.nonConflicting.size() == 2);

        auto streamingX = makeTrack("engine", "sX", "/media/B/Engine Library", {});
        auto streamingY = makeTrack("engine", "sY", "/media/B/Engine Library", {});
        streamingX.streamingSource = streamingY.streamingSource = "TIDAL";
        auto streamingSplit = CrossSourceConflictDetector::detect(
            {makePlan(rb, streamingX, {hot(1, 1000.0)}), makePlan(ol, streamingY, {hot(1, 9000.0)})});
        assert(streamingSplit.conflicts.empty() && streamingSplit.nonConflicting.size() == 2);
        std::cout << "case e (targets without a file, or streaming ones, are never grouped into a conflict) OK\n";
    }

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
        const auto choices = seabass::domain::CrossSourceConflictDetector::takeHotCueChoices(plans, seabass::application::normalizedPathKey);
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
        const auto choices = seabass::domain::CrossSourceConflictDetector::takeHotCueChoices(plans, seabass::application::normalizedPathKey);
        assert(choices.size() == 1 && "one card for one track, not one per pair");
        const bool hasRekordbox = choices[0].sourceA.format == "rekordbox" || choices[0].sourceB.format == "rekordbox";
        assert(hasRekordbox && "the kept choice is the rekordbox one; its write mirrors into OneLibrary");
        assert(plans.size() == 1 && plans[0].match.trackA.sourceId == "r2"
               && "every other plan for the undecided file waits; other files are untouched");
        std::cout << "case hot-cue-choice-three-formats (one card per file, and nothing else writes it meanwhile) OK\n";

        // Without rekordbox on the stick the OneLibrary choice is the card.
        std::vector<SyncPlan> onlyOneLibrary = {viaOneLibrary};
        const auto alone = seabass::domain::CrossSourceConflictDetector::takeHotCueChoices(onlyOneLibrary, seabass::application::normalizedPathKey);
        assert(alone.size() == 1 && onlyOneLibrary.empty());
        std::cout << "case hot-cue-choice-onelibrary-only (a OneLibrary choice stands when it is the only one) OK\n";
    }

    // The sixth review's finding: the file was keyed on trackA's raw path,
    // but each reader spells a path its own way and a title match can pair
    // two different paths. Keyed that way, the choice sat under one spelling
    // and the other pair's plan under another, so nothing held the plan back
    // and the track got a second card.
    {
        using seabass::domain::CuePoint;
        using seabass::domain::SyncPlan;
        const auto track = [](const std::string &format, const std::string &id, const std::string &path) {
            seabass::domain::Track t;
            t.format = format;
            t.sourceId = id;
            t.filePath = path;
            return t;
        };
        const auto choiceBetween = [](const seabass::domain::Track &a, const seabass::domain::Track &b) {
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
        const auto plainBetween = [](const seabass::domain::Track &a, const seabass::domain::Track &b) {
            SyncPlan p;
            p.kind = SyncPlan::Kind::AOnly;
            p.direction = SyncPlan::Direction::ToB;
            p.match.trackA = a;
            p.match.trackB = b;
            return p;
        };

        // Windows: the reviewer's spellings of one file. Keyed on trackA
        // alone, the choice sat under rekordbox's spelling and the
        // Engine->OneLibrary plan under Engine's, so the plan went through.
        // (Both sides of that plan name the same Engine row as the choice,
        // so it is keying both sides that catches this; the normalized key
        // additionally joins spellings no side shares.)
        {
            const std::string rekordboxPath = "E:\\/Contents/song.mp3";
            const std::string enginePath = "E:\\Contents\\song.mp3";
            const std::string oneLibraryPath = "E:\\Contents\\Song.mp3";
            assert(rekordboxPath != enginePath);
            const auto rb = track("rekordbox", "r1", rekordboxPath);
            const auto en = track("engine", "e1", enginePath);
            const auto ol = track("onelibrary", "o1", oneLibraryPath);
            const auto other = track("engine", "e2", "E:\\Contents\\other.mp3");

            // OneLibrary has no hot cues: only the rekordbox pair asks, and the
            // Engine->OneLibrary plan must wait for that answer.
            std::vector<SyncPlan> plans = {plainBetween(en, ol), choiceBetween(rb, en),
                                           plainBetween(other, track("onelibrary", "o2", "E:/Contents/other.mp3"))};
            const auto choices = seabass::domain::CrossSourceConflictDetector::takeHotCueChoices(
                plans, seabass::application::normalizedPathKey);
            assert(choices.size() == 1);
            assert(plans.size() == 1 && plans[0].match.trackA.sourceId == "e2"
                   && "the Engine->OneLibrary plan for the undecided file waits, whatever its path spelling");

            // Both pairs ask: still one card, the rekordbox one.
            std::vector<SyncPlan> both = {choiceBetween(en, ol), choiceBetween(rb, en)};
            const auto one = seabass::domain::CrossSourceConflictDetector::takeHotCueChoices(
                both, seabass::application::normalizedPathKey);
            assert(one.size() == 1 && both.empty());
            assert(one[0].sourceA.format == "rekordbox" && "one card per track across path spellings");
        }
        std::cout << "case hot-cue-choice-path-spellings (Windows separators do not split one track) OK\n";

        // A title match pairs two different paths: rekordbox's file on one
        // drive, Engine's copy on another. The Engine<->OneLibrary plan names
        // only Engine's path and OneLibrary's (rekordbox's) path, never the
        // choice's trackA key alone -- it must still wait.
        {
            const auto rb = track("rekordbox", "r1", "/media/A/Contents/song.mp3");
            const auto en = track("engine", "e1", "/media/B/Music/song.mp3");
            const auto ol = track("onelibrary", "o1", "/media/A/Contents/song.mp3");

            std::vector<SyncPlan> enginePathOnly = {choiceBetween(rb, en),
                                                    plainBetween(en, track("onelibrary", "o9", "/media/C/x.mp3"))};
            seabass::domain::CrossSourceConflictDetector::takeHotCueChoices(enginePathOnly,
                                                                             seabass::application::normalizedPathKey);
            assert(enginePathOnly.empty() && "a plan sharing only the Engine side's path still waits");

            std::vector<SyncPlan> plans = {choiceBetween(rb, en), plainBetween(en, ol)};
            const auto choices = seabass::domain::CrossSourceConflictDetector::takeHotCueChoices(
                plans, seabass::application::normalizedPathKey);
            assert(choices.size() == 1 && plans.empty());
        }
        std::cout << "case hot-cue-choice-title-match (a choice holds both of its paths) OK\n";

        // Two choices with different keys, joined only through the Engine
        // path they share. The Engine<->OneLibrary choice comes first, so its
        // group already exists when the rekordbox choice arrives and must be
        // merged into it: without the join there would be two cards. (The
        // plain plan waits either way -- holding back only asks whether a key
        // belongs to any open choice -- so the card count is the assertion
        // that guards the join.)
        {
            const auto rb = track("rekordbox", "r1", "/media/A/Contents/song.mp3");
            const auto en = track("engine", "e1", "/media/B/Music/song.mp3");
            const auto ol = track("onelibrary", "o1", "/media/C/Contents/song.mp3");
            std::vector<SyncPlan> plans = {choiceBetween(en, ol), choiceBetween(rb, en),
                                           plainBetween(track("engine", "e7", "/media/D/x.mp3"), ol)};
            const auto choices = seabass::domain::CrossSourceConflictDetector::takeHotCueChoices(
                plans, seabass::application::normalizedPathKey);
            assert(choices.size() == 1 && "two choices linked through Engine's path are one track");
            assert(choices[0].sourceA.format == "rekordbox");
            assert(plans.empty() && "a plan naming a path of an open choice waits");
        }
        std::cout << "case hot-cue-choice-joined-groups (choices with different keys meet through a shared path) OK\n";

        // Streaming tracks have no file of their own, and their paths can all
        // be the Engine Library folder. Two rekordbox tracks title-matched to
        // two different streaming tracks are two cards, and a plan for a
        // third streaming track is not held back by either.
        {
            auto streaming = [&](const std::string &id) {
                auto t = track("engine", id, "/media/B/Engine Library");
                t.streamingSource = "TIDAL";
                return t;
            };
            std::vector<SyncPlan> plans = {
                choiceBetween(track("rekordbox", "r1", "/media/A/Contents/one.mp3"), streaming("s1")),
                choiceBetween(track("rekordbox", "r2", "/media/A/Contents/two.mp3"), streaming("s2")),
                plainBetween(track("rekordbox", "r3", "/media/A/Contents/three.mp3"), streaming("s3"))};
            const auto choices = seabass::domain::CrossSourceConflictDetector::takeHotCueChoices(
                plans, seabass::application::normalizedPathKey);
            assert(choices.size() == 2 && "two tracks, two cards, even though their streaming paths are equal");
            assert(plans.size() == 1 && plans[0].match.trackA.sourceId == "r3"
                   && "a streaming track's shared path holds nothing back");
        }
        std::cout << "case hot-cue-choice-streaming (streaming tracks share no file) OK\n";
    }

    std::cout << "All cross_source_sync_conflict_test cases passed.\n";
    return 0;
}
