// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <cmath>
#include <iostream>

#include "domain/matching_policy.hpp"
#include "domain/sync_planning.hpp"
#include "domain/track_matching.hpp"

using namespace seabass::domain;
using namespace std::chrono;

Track makeTrack(std::string id, std::string filename, double duration, std::vector<CuePoint> cues,
                 std::string title = "", std::string artist = "")
{
    Track t;
    t.sourceId = std::move(id);
    t.filename = std::move(filename);
    t.durationSeconds = duration;
    t.cues = std::move(cues);
    t.title = std::move(title);
    t.artist = std::move(artist);
    return t;
}

int main()
{
    auto now = system_clock::now();

    // Matching: same filename + duration across formats pairs up.
    {
        std::vector<Track> rekordbox = {makeTrack("r1", "song.mp3", 200.0, {})};
        std::vector<Track> engine = {makeTrack("e1", "song.mp3", 200.5, {})};
        auto matches = TrackMatcher::match(rekordbox, engine);
        assert(matches.size() == 1);
        assert(matches[0].trackA.sourceId == "r1");
        assert(matches[0].trackB.sourceId == "e1");
        std::cout << "case 1 (matching) OK\n";
    }

    // No match when filenames differ and there's no title/artist metadata
    // to fall back on.
    {
        std::vector<Track> rekordbox = {makeTrack("r1", "a.mp3", 200.0, {})};
        std::vector<Track> engine = {makeTrack("e1", "b.mp3", 200.0, {})};
        auto matches = TrackMatcher::match(rekordbox, engine);
        assert(matches.empty());
        std::cout << "case 2 (no match, different filenames, no metadata) OK\n";
    }

    // Title+artist is the primary matching signal: filenames can legitimately
    // differ across formats (e.g. a playlist index embedded in the
    // filename), but matching title+artist still pairs the tracks up.
    {
        std::vector<Track> rekordbox = {
            makeTrack("r1", "01 - song.mp3", 200.0, {}, "Song", "Artist")};
        std::vector<Track> engine = {
            makeTrack("e1", "song (export).mp3", 200.5, {}, "Song", "Artist")};
        auto matches = TrackMatcher::match(rekordbox, engine);
        assert(matches.size() == 1);
        assert(matches[0].trackA.sourceId == "r1");
        assert(matches[0].trackB.sourceId == "e1");
        std::cout << "case 2b (matching by title+artist despite differing filenames) OK\n";
    }

    // Title+artist matches but duration is wildly different (e.g. a cover
    // version, or coincidentally identical metadata) -> not treated as the
    // same track.
    {
        std::vector<Track> rekordbox = {
            makeTrack("r1", "a.mp3", 200.0, {}, "Song", "Artist")};
        std::vector<Track> engine = {
            makeTrack("e1", "b.mp3", 400.0, {}, "Song", "Artist")};
        auto matches = TrackMatcher::match(rekordbox, engine);
        assert(matches.empty());
        std::cout << "case 2c (title+artist match but duration too different -> no match) OK\n";
    }

    // rekordbox has cues, engine doesn't -> propagate to engine.
    {
        SyncMatch m{makeTrack("r1", "song.mp3", 200.0,
                              {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"}}),
                    makeTrack("e1", "song.mp3", 200.0, {})};
        auto plan = SyncPlanner::plan(m, now, now);
        assert(plan.kind == SyncPlan::Kind::AOnly);
        assert(plan.direction == SyncPlan::Direction::ToB);
        assert(plan.cuesToApply.size() == 1);
        std::cout << "case 3 (rekordbox-only -> to engine) OK\n";
    }

    // Engine has cues, rekordbox doesn't -> would propagate to rekordbox (unwritable, but planned).
    {
        SyncMatch m{makeTrack("r1", "song.mp3", 200.0, {}),
                    makeTrack("e1", "song.mp3", 200.0,
                              {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"}})};
        auto plan = SyncPlanner::plan(m, now, now);
        assert(plan.kind == SyncPlan::Kind::BOnly);
        assert(plan.direction == SyncPlan::Direction::ToA);
        std::cout << "case 4 (engine-only -> to rekordbox) OK\n";
    }

    // Both consistent -> no-op.
    {
        SyncMatch m{makeTrack("r1", "song.mp3", 200.0,
                              {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"}}),
                    makeTrack("e1", "song.mp3", 200.0,
                              {CuePoint{CuePoint::Kind::Hot, 1, 1000.4, "#FF0000", "drop"}})};
        auto plan = SyncPlanner::plan(m, now, now);
        assert(plan.kind == SyncPlan::Kind::AlreadyConsistent);
        std::cout << "case 5 (already consistent) OK\n";
    }

    // No clock decides a conflict (2026-10-04): the catalog files' mtimes
    // used to pick the side, newer one way and then the other. Both ways
    // round it is the DJ's choice now, and nothing is applied meanwhile.
    for (const bool rekordboxNewer : {true, false}) {
        SyncMatch m{makeTrack("r1", "song.mp3", 200.0,
                              {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"}}),
                    makeTrack("e1", "song.mp3", 200.0,
                              {CuePoint{CuePoint::Kind::Hot, 1, 5000.0, "#00FF00", "intro"}})};
        auto plan = rekordboxNewer ? SyncPlanner::plan(m, now, now - hours(1)) : SyncPlanner::plan(m, now - hours(1), now);
        assert(plan.kind == SyncPlan::Kind::Conflict);
        assert(plan.needsChoice);
        assert(plan.direction == SyncPlan::Direction::None && plan.cuesToApply.empty());
    }
    std::cout << "case 6/7 (a conflict is never settled by the catalog files' mtimes) OK\n";

    // The wipe this planner used to do. rekordbox has three memory cues,
    // Engine can only hold one, and after a sync Engine holds the first of
    // them. Same hot cues. That is agreement: Engine's one memory cue is
    // among rekordbox's. It used to read as a conflict, m.db was the newer
    // file (the sync had just written it), and Engine's single memory cue
    // replaced rekordbox's three -- on every sync after the first.
    {
        Track r = makeTrack("r1", "song.mp3", 200.0,
                            {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 2000.0, "", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 60000.0, "", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 120000.0, "", ""}});
        r.format = "rekordbox";
        Track e = makeTrack("e1", "song.mp3", 200.0,
                            {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 2000.0, "", ""}});
        e.format = "engine";
        auto plan = SyncPlanner::plan(SyncMatch{r, e}, now - hours(1), now);
        // Engine is written, never rekordbox: it lacks pads for the memory
        // cues at 60 s and 120 s, the way Engine DJ's own import gives them
        // pads. Its main cue stays, and rekordbox keeps all three.
        assert(plan.kind == SyncPlan::Kind::Conflict);
        assert(plan.direction == SyncPlan::Direction::ToB);
        std::vector<double> pads;
        int mains = 0;
        for (const auto &cue : plan.cuesToApply) {
            if (cue.kind == CuePoint::Kind::Hot) {
                pads.push_back(cue.positionMs);
            } else {
                mains++;
                assert(cue.positionMs == 2000.0 && "Engine's own main cue is kept");
            }
        }
        assert(mains == 1);
        assert((pads == std::vector<double>{1000.0, 2000.0, 60000.0, 120000.0}) && "memory cues take the free pads");
        e.cues = plan.cuesToApply;
        auto after = SyncPlanner::plan(SyncMatch{r, e}, now - hours(1), now);
        assert(after.kind == SyncPlan::Kind::AlreadyConsistent && "one sync settles it");
        std::cout << "case 8 (rekordbox's memory cues become Engine hot cues, and the pair then agrees) OK\n";
    }

    // A genuine hot cue conflict with Engine newer still goes to rekordbox
    // -- but rekordbox keeps all three of its memory cues.
    {
        Track r = makeTrack("r1", "song.mp3", 200.0,
                            {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 2000.0, "", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 60000.0, "", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 120000.0, "", ""}});
        r.format = "rekordbox";
        Track e = makeTrack("e1", "song.mp3", 200.0,
                            {CuePoint{CuePoint::Kind::Hot, 1, 9000.0, "#00FF00", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 2000.0, "", ""}});
        e.format = "engine";
        auto plan = SyncPlanner::plan(SyncMatch{r, e}, now - hours(1), now);
        assert(plan.kind == SyncPlan::Kind::Conflict);
        assert(plan.needsChoice && plan.direction == SyncPlan::Direction::None);
        int hot = 0;
        int memory = 0;
        for (const auto &cue : plan.cuesIfBWins) {
            if (cue.kind == CuePoint::Kind::Hot) {
                hot++;
                assert(cue.positionMs == 9000.0);  // Engine's hot cue, if Engine is picked
            } else {
                memory++;
            }
        }
        assert(hot == 1);
        assert(memory == 3);  // none of rekordbox's memory cues is taken away
        std::cout << "case 9 (picking Engine's hot cues keeps every memory cue) OK\n";
    }

    // Nor by each track's own edit time. The stick's clocks are not to be
    // trusted: an XDJ-RX2 was measured stamping 2017, a Prime 4 two hours
    // off, and FAT times move with the time zone. rekordbox edited "in
    // 2017" and Engine "in 2026", and the reverse, catalog files either
    // way: always a choice, never a write.
    for (const bool rekordboxIn2017 : {true, false}) {
        for (const bool rekordboxFileNewer : {true, false}) {
            Track r = makeTrack("r1", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""}});
            r.format = "rekordbox";
            r.metadataModifiedAt = rekordboxIn2017 ? 1'500'000'000 : 1'790'000'000;  // 2017-07 / 2026-09
            Track e = makeTrack("e1", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 5000.0, "#FF0000", ""}});
            e.format = "engine";
            e.metadataModifiedAt = rekordboxIn2017 ? 1'790'000'000 : 1'500'000'000;
            auto plan = rekordboxFileNewer ? SyncPlanner::plan(SyncMatch{r, e}, now, now - hours(1))
                                           : SyncPlanner::plan(SyncMatch{r, e}, now - hours(1), now);
            assert(plan.kind == SyncPlan::Kind::Conflict);
            assert(plan.needsChoice && plan.direction == SyncPlan::Direction::None && plan.cuesToApply.empty());
            assert(plan.reason == SyncPlan::Reason::PadsDiffer);
            assert(plan.reasonText == "Pad 1: rekordbox 0:01.000, Engine 0:05.000");
        }
    }
    std::cout << "case 10/11 (edit times decide nothing, whatever they say) OK\n";

    // The review finding: Engine's only memory cue destroyed. rekordbox has
    // memory cues at 10s and 20s, Engine one at 15s, and rekordbox is newer
    // with a differing hot cue. The plan goes onto Engine -- and must not
    // send it a union whose earliest (10s) would replace Engine's 15s, a cue
    // that exists nowhere else.
    {
        Track r = makeTrack("r1", "song.mp3", 200.0,
                            {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 10000.0, "", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 20000.0, "", ""}});
        r.format = "rekordbox";
        r.metadataModifiedAt = 1'800'000'000;
        Track e = makeTrack("e1", "song.mp3", 200.0,
                            {CuePoint{CuePoint::Kind::Hot, 1, 9000.0, "#00FF00", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 15000.0, "", ""}});
        e.format = "engine";
        e.metadataModifiedAt = 1'700'000'000;
        auto plan = SyncPlanner::plan(SyncMatch{r, e}, now, now);
        assert(plan.kind == SyncPlan::Kind::Conflict);
        assert(plan.needsChoice);
        std::vector<double> memory;
        std::vector<double> pads;
        for (const auto &cue : plan.cuesIfAWins) {  // picking rekordbox writes Engine
            if (cue.kind == CuePoint::Kind::Hot) {
                pads.push_back(cue.positionMs);
            } else {
                memory.push_back(cue.positionMs);
            }
        }
        assert((pads == std::vector<double>{1000.0, 10000.0, 20000.0}) && "rekordbox's hot cue, then its memory cues on pads");
        assert(memory.size() == 1 && memory[0] == 15000.0 && "Engine keeps its own memory cue");
        std::cout << "case 12 (picking rekordbox's hot cues keeps Engine's only memory cue) OK\n";
    }

    // ...and the next sync carries it across. Hot cues now agree, Engine's
    // 15s is not among rekordbox's memory cues, and Engine happens to be the
    // newer side. The plan must still write rekordbox, the side that can
    // hold every memory cue, with the union -- never Engine.
    {
        Track r = makeTrack("r1", "song.mp3", 200.0,
                            {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 10000.0, "", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 20000.0, "", ""}});
        r.format = "rekordbox";
        r.metadataModifiedAt = 1'700'000'000;
        Track e = makeTrack("e1", "song.mp3", 200.0,
                            {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 15000.0, "", ""}});
        e.format = "engine";
        e.metadataModifiedAt = 1'800'000'000;
        auto plan = SyncPlanner::plan(SyncMatch{r, e}, now, now);
        assert(plan.kind == SyncPlan::Kind::Conflict);
        assert(plan.direction == SyncPlan::Direction::ToA && "memory cues resolve onto rekordbox, not Engine");
        int memory = 0;
        bool has15 = false;
        for (const auto &cue : plan.cuesToApply) {
            if (cue.kind == CuePoint::Kind::Memory) {
                memory++;
                has15 = has15 || cue.positionMs == 15000.0;
            }
        }
        assert(memory == 3 && has15);
        std::cout << "case 13 (a memory-only difference writes the side that can hold every memory cue) OK\n";

        // After that write Engine's main cue is among rekordbox's three, and
        // the next sync gives Engine pads for the two it still lacks. Then
        // the pair is consistent, with no memory cue lost on either side.
        r.cues = plan.cuesToApply;
        auto next = SyncPlanner::plan(SyncMatch{r, e}, now, now);
        assert(next.direction == SyncPlan::Direction::ToB && "now Engine is the side missing something");
        e.cues = next.cuesToApply;
        auto after = SyncPlanner::plan(SyncMatch{r, e}, now, now);
        assert(after.kind == SyncPlan::Kind::AlreadyConsistent);
        std::cout << "case 13b (two syncs settle it with no memory cue lost) OK\n";
    }

    // The third review's finding: an Engine track with no memory cue at all
    // never got one. Hot cues agree, rekordbox has memory cues at 2s and 60s,
    // Engine has none. Writing rekordbox would hand it back exactly what it
    // has and repeat on every sync; Engine is the side missing something,
    // and has nothing to lose.
    {
        Track r = makeTrack("r1", "song.mp3", 200.0,
                            {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 2000.0, "", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 60000.0, "", ""}});
        r.format = "rekordbox";
        r.metadataModifiedAt = 1'800'000'000;  // rekordbox newer: must not matter here
        Track e = makeTrack("e1", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""}});
        e.format = "engine";
        e.metadataModifiedAt = 1'700'000'000;
        auto plan = SyncPlanner::plan(SyncMatch{r, e}, now, now);
        assert(plan.kind == SyncPlan::Kind::Conflict);
        assert(plan.direction == SyncPlan::Direction::ToB && "an Engine side with no memory cue is the one written");
        std::cout << "case 14 (an Engine track with no memory cue is given one) OK\n";

        // Engine's writer keeps the earliest memory cue it is given. After
        // that write the pair agrees: nothing left to rewrite next time.
        std::vector<CuePoint> engineAfter;
        const CuePoint *earliest = nullptr;
        for (const auto &cue : plan.cuesToApply) {
            if (cue.kind == CuePoint::Kind::Hot) {
                engineAfter.push_back(cue);
            } else if (!earliest || cue.positionMs < earliest->positionMs) {
                earliest = &cue;
            }
        }
        assert(earliest != nullptr && earliest->positionMs == 2000.0);
        engineAfter.push_back(*earliest);
        e.cues = engineAfter;
        auto after = SyncPlanner::plan(SyncMatch{r, e}, now, now);
        assert(after.kind == SyncPlan::Kind::AlreadyConsistent && "one sync settles it; no endless conflict");
        std::cout << "case 14b (the empty Engine side settles in one sync) OK\n";
    }

    // Your decision after the third review: a real hot cue conflict is asked,
    // not settled by a clock. Both sides have hot cues and they differ.
    {
        Track r = makeTrack("r1", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""}});
        r.format = "rekordbox";
        r.metadataModifiedAt = 1'700'000'000;
        Track e = makeTrack("e1", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 5000.0, "#FF0000", ""}});
        e.format = "engine";
        e.metadataModifiedAt = 1'800'000'000;  // e.g. rated in Engine DJ after the rekordbox cue edit
        auto plan = SyncPlanner::plan(SyncMatch{r, e}, now, now);
        assert(plan.kind == SyncPlan::Kind::Conflict);
        assert(plan.needsChoice && "both sides have different hot cues: the DJ chooses");
        // Both choices are ready to write, each with its own hot cues.
        assert(!plan.cuesIfAWins.empty() && plan.cuesIfAWins.front().positionMs == 1000.0);
        assert(!plan.cuesIfBWins.empty() && plan.cuesIfBWins.front().positionMs == 5000.0);
        // No suggestion: the newer side used to be one, and a caller that
        // ignored the flag wrote it.
        assert(plan.direction == SyncPlan::Direction::None && plan.cuesToApply.empty());
        std::cout << "case 15 (differing hot cues on both sides need a choice, with both options prepared) OK\n";
    }

    // Only one side has hot cues: nothing to choose, nothing at risk. That
    // side wins even when the other is newer.
    {
        Track r = makeTrack("r1", "song.mp3", 200.0,
                            {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 2000.0, "", ""}});
        r.format = "rekordbox";
        r.metadataModifiedAt = 1'700'000'000;
        Track e = makeTrack("e1", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Memory, 0, 9000.0, "", ""}});
        e.format = "engine";
        e.metadataModifiedAt = 1'800'000'000;
        auto plan = SyncPlanner::plan(SyncMatch{r, e}, now, now);
        assert(plan.kind == SyncPlan::Kind::Conflict);
        assert(!plan.needsChoice);
        assert(plan.direction == SyncPlan::Direction::ToB && "the side with hot cues wins; none are erased");
        std::cout << "case 16 (hot cues on one side only are copied across without asking) OK\n";
    }

    // Counting a write the way the page shows it: a hot cue already on the
    // same pad at the same place is kept, the rest are gained.
    {
        std::vector<CuePoint> current = {CuePoint{CuePoint::Kind::Hot, 1, 5000.0, "", ""}};
        std::vector<CuePoint> written = {CuePoint{CuePoint::Kind::Hot, 1, 5000.0, "", ""},
                                         CuePoint{CuePoint::Kind::Hot, 2, 20000.0, "", ""},
                                         CuePoint{CuePoint::Kind::Hot, 3, 40000.0, "", ""},
                                         CuePoint{CuePoint::Kind::Hot, 4, 60000.0, "", ""}};
        CueChange change = describeCueChange(current, written);
        assert(change.gainedHot == 3 && "four written, one of them already there");
        assert(change.keptHot == 1);
        assert(change.droppedHot == 0);
        std::cout << "case 17 (a hot cue already there is kept, not gained) OK\n";
    }

    // A cue moved to another pad is not the same hot cue: one gained, one
    // dropped. A memory cue a rounding away is still the same cue.
    {
        std::vector<CuePoint> current = {CuePoint{CuePoint::Kind::Hot, 2, 5000.0, "", ""},
                                         CuePoint{CuePoint::Kind::Memory, 0, 9000.0, "", ""}};
        std::vector<CuePoint> written = {CuePoint{CuePoint::Kind::Hot, 1, 5000.0, "", ""},
                                         CuePoint{CuePoint::Kind::Memory, 0, 9001.0, "", ""}};
        CueChange change = describeCueChange(current, written);
        assert(change.gainedHot == 1 && change.droppedHot == 1 && change.keptHot == 0);
        assert(change.keptMemory == 1 && change.gainedMemory == 0 && change.droppedMemory == 0);
        std::cout << "case 18 (a moved pad is dropped and gained; a drifted memory cue is kept) OK\n";
    }

    // Colour is extra information about a cue, not what a cue is. Two
    // sides holding the same cue in another shade used to be a plan --
    // a conflict to weigh, an offer to write, a row that came back after
    // every sync, because the shade was all that ever differed.
    {
        SyncMatch m{makeTrack("r1", "song.mp3", 200.0,
                              {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"}}),
                    makeTrack("e1", "song.mp3", 200.0,
                              {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#00FF00", ""}})};
        auto plan = SyncPlanner::plan(m, now, now);
        assert(plan.kind == SyncPlan::Kind::AlreadyConsistent);
        assert(plan.direction == SyncPlan::Direction::None);
        assert(!plan.needsChoice);
        std::cout << "case (a colour difference is not a disagreement) OK\n";
    }

    // Nor is a colour on one side only: rekordbox colours hot cue 1, the
    // OneLibrary export left it bare. Colour is decoration, so the pair is
    // consistent, not a plan and not a choice. This listed whole libraries
    // as conflicts on Sync Cue Points.
    {
        SyncMatch m{makeTrack("r1", "song.mp3", 200.0,
                              {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""}}),
                    makeTrack("e1", "song.mp3", 200.0,
                              {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "", ""}})};
        auto plan = SyncPlanner::plan(m, now, now);
        assert(plan.kind == SyncPlan::Kind::AlreadyConsistent);
        assert(plan.direction == SyncPlan::Direction::None);
        assert(!plan.needsChoice);
        std::cout << "case (a colour on one side only is not a disagreement either) OK\n";
    }

    // But it is not thrown away either. A cue written onto a side that
    // already had a colour for it keeps that colour: the format the cue
    // came from may simply have no way to say (an Engine memory cue never
    // does, and the rekordbox writer records no comment at all), and
    // silence must not paint over what is there.
    {
        SyncMatch m{makeTrack("r1", "song.mp3", 200.0,
                              {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"},
                               CuePoint{CuePoint::Kind::Hot, 2, 2000.0, "#0000FF", ""}}),
                    makeTrack("e1", "song.mp3", 200.0,
                              {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "", ""},
                               CuePoint{CuePoint::Kind::Hot, 3, 9000.0, "", ""}})};
        auto plan = SyncPlanner::plan(m, now, now);
        assert(plan.needsChoice && "the slots really differ, which is a choice");
        // Whichever way the user goes, a colour the target holds survives.
        for (const CuePoint &cue : plan.cuesIfBWins) {
            if (cue.kind == CuePoint::Kind::Hot && cue.hotCueNumber == 1) {
                assert(cue.color == "#FF0000" && "writing Engine's colourless cue onto rekordbox keeps the red");
            }
        }
        std::cout << "case (a colour is carried onto the side being written) OK\n";
    }

    // WHALESHARK2, 2026-10-02: rekordbox carries a memory cue at 0:00 on
    // most tracks (179 of them on that stick), Engine stores "no main cue"
    // as offset 0, and every sync offered the same 26 tracks again. Two
    // rules now keep that from happening, one per preference setting.
    //
    // With "Ignore cues at 0:00" on (the default), such a cue is not part
    // of the sync at all: not compared, so the pair is consistent...
    {
        MatchingPolicy::set(MatchingPolicy::DefaultExactMatchSeconds, MatchingPolicy::DefaultCompareAudioSeconds,
                            true);
        Track r = makeTrack("r1", "song.mp3", 200.0,
                            {CuePoint{CuePoint::Kind::Hot, 1, 30000.0, "#FF0000", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 0.0, "", ""}});
        r.format = "rekordbox";
        Track e = makeTrack("e1", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 30000.0, "#FF0000", ""}});
        e.format = "engine";
        auto plan = SyncPlanner::plan(SyncMatch{r, e}, now, now);
        assert(plan.kind == SyncPlan::Kind::AlreadyConsistent && "an ignored cue is no difference");
        assert(plan.match.trackA.cues.size() == 2 && "the plan still carries the track as scanned");
        std::cout << "case (an ignored cue at 0:00 is not a difference) OK\n";

        // ...and not copied: a hot cue inside the first second stays where
        // it is, the real one travels.
        Track r2 = makeTrack("r1", "song.mp3", 200.0,
                             {CuePoint{CuePoint::Kind::Hot, 1, 500.0, "#FF0000", ""},
                              CuePoint{CuePoint::Kind::Hot, 2, 30000.0, "#00FF00", ""},
                              CuePoint{CuePoint::Kind::Memory, 0, 0.0, "", ""}});
        r2.format = "rekordbox";
        Track e2 = makeTrack("e1", "song.mp3", 200.0, {});
        e2.format = "engine";
        auto copy = SyncPlanner::plan(SyncMatch{r2, e2}, now, now);
        assert(copy.kind == SyncPlan::Kind::AOnly && copy.direction == SyncPlan::Direction::ToB);
        assert(copy.cuesToApply.size() == 1 && copy.cuesToApply[0].hotCueNumber == 2
               && "only the cue the preference does not ignore is written");
        std::cout << "case (an ignored cue is not copied) OK\n";

        // A track with nothing but ignored cues against an empty one: no
        // write, rather than a write of nothing.
        Track r3 = makeTrack("r1", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Memory, 0, 0.0, "", ""}});
        r3.format = "rekordbox";
        auto none = SyncPlanner::plan(SyncMatch{r3, e2}, now, now);
        assert(none.direction == SyncPlan::Direction::None && "nothing to write");
        std::cout << "case (only ignored cues on one side is nothing to sync) OK\n";
        MatchingPolicy::reset();
    }

    // With the preference off the cue counts, but Engine still cannot hold
    // it: writing a memory cue at 0:00 to Engine stores "no cue". So it is
    // never something Engine lacks, and never in what is written to Engine.
    {
        MatchingPolicy::set(MatchingPolicy::DefaultExactMatchSeconds, MatchingPolicy::DefaultCompareAudioSeconds,
                            false);
        Track r = makeTrack("r1", "song.mp3", 200.0,
                            {CuePoint{CuePoint::Kind::Hot, 1, 30000.0, "#FF0000", ""},
                             CuePoint{CuePoint::Kind::Memory, 0, 0.0, "", ""}});
        r.format = "rekordbox";
        Track e = makeTrack("e1", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 30000.0, "#FF0000", ""}});
        e.format = "engine";
        // The cue at 0:00 counts, so it goes onto an Engine pad, as it
        // would in Engine DJ's own import. It cannot be the main cue (0 is
        // "no cue" there), so Engine gets none, and after the write the
        // pair agrees.
        auto plan = SyncPlanner::plan(SyncMatch{r, e}, now, now);
        assert(plan.direction == SyncPlan::Direction::ToB);
        int mains = 0;
        for (const auto &cue : plan.cuesToApply) {
            mains += cue.kind == CuePoint::Kind::Memory ? 1 : 0;
        }
        assert(mains == 0 && "a cue at 0:00 is never written as the main cue");
        e.cues = plan.cuesToApply;
        assert(SyncPlanner::plan(SyncMatch{r, e}, now, now).kind == SyncPlan::Kind::AlreadyConsistent);
        std::cout << "case (a cue at 0:00 that counts becomes an Engine pad, never the main cue) OK\n";

        // One at 0:00 and one at 1:00: both take pads, the main cue is
        // the one at 1:00, and one sync settles it.
        Track r2 = makeTrack("r1", "song.mp3", 200.0,
                             {CuePoint{CuePoint::Kind::Hot, 1, 30000.0, "#FF0000", ""},
                              CuePoint{CuePoint::Kind::Memory, 0, 0.0, "", ""},
                              CuePoint{CuePoint::Kind::Memory, 0, 60000.0, "", ""}});
        r2.format = "rekordbox";
        Track e2 = makeTrack("e1", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 30000.0, "#FF0000", ""}});
        e2.format = "engine";
        auto onto = SyncPlanner::plan(SyncMatch{r2, e2}, now, now);
        assert(onto.kind == SyncPlan::Kind::Conflict && onto.direction == SyncPlan::Direction::ToB);
        std::vector<double> memory;
        std::vector<double> pads;
        for (const auto &cue : onto.cuesToApply) {
            (cue.kind == CuePoint::Kind::Memory ? memory : pads).push_back(cue.positionMs);
        }
        assert((pads == std::vector<double>{30000.0, 0.0, 60000.0}));
        assert(memory.size() == 1 && memory[0] == 60000.0 && "the main cue is the earliest Engine can hold");
        e2.cues = onto.cuesToApply;
        auto after = SyncPlanner::plan(SyncMatch{r2, e2}, now, now);
        assert(after.kind == SyncPlan::Kind::AlreadyConsistent && "one sync settles it");
        std::cout << "case (the main cue is the earliest memory cue Engine can hold) OK\n";

        // The reverse direction keeps rekordbox's own cue at 0:00: a sync
        // onto rekordbox writes the union, and rekordbox can hold it.
        Track e3 = makeTrack("e1", "song.mp3", 200.0,
                             {CuePoint{CuePoint::Kind::Hot, 1, 30000.0, "#FF0000", ""},
                              CuePoint{CuePoint::Kind::Memory, 0, 90000.0, "", ""}});
        e3.format = "engine";
        auto back = SyncPlanner::plan(SyncMatch{r, e3}, now, now);
        assert(back.direction == SyncPlan::Direction::ToA && "rekordbox lacks Engine's cue at 1:30");
        int atStart = 0;
        for (const auto &cue : back.cuesToApply) {
            if (cue.kind == CuePoint::Kind::Memory && cue.positionMs == 0.0) {
                atStart++;
            }
        }
        assert(atStart == 1 && "rekordbox keeps its cue at 0:00 when the preference says it counts");
        std::cout << "case (a sync onto rekordbox keeps its cue at 0:00) OK\n";
        MatchingPolicy::reset();
    }

    // WHALESHARK2, "Voices In My Head": rekordbox has five hot cues and a
    // memory cue under hot cue 3; Engine has the five pads and that main
    // cue. In sync, not "give the marker pad 6".
    {
        std::vector<CuePoint> five = {CuePoint{CuePoint::Kind::Hot, 1, 11333.0, "#00FF00", ""},
                                      CuePoint{CuePoint::Kind::Hot, 2, 26332.0, "#00FF00", ""},
                                      CuePoint{CuePoint::Kind::Hot, 3, 52583.0, "#00FF00", ""},
                                      CuePoint{CuePoint::Kind::Hot, 4, 97583.0, "#00FF00", ""},
                                      CuePoint{CuePoint::Kind::Hot, 5, 127583.0, "#00FF00", ""}};
        Track r = makeTrack("r1", "song.mp3", 146.0, five);
        r.cues.push_back(CuePoint{CuePoint::Kind::Memory, 0, 52583.0, "#000000", ""});
        r.format = "rekordbox";
        Track e = makeTrack("e1", "song.mp3", 146.0, five);
        e.cues[1].positionMs = 26331.0;  // a millisecond of rounding
        e.cues.push_back(CuePoint{CuePoint::Kind::Memory, 0, 52583.0, "", ""});
        e.format = "engine";
        auto plan = SyncPlanner::plan(SyncMatch{r, e}, now, now);
        assert(plan.kind == SyncPlan::Kind::AlreadyConsistent && "the marker is on pad 3 already");
        std::cout << "case (a marker under a hot cue is in sync, not a sixth pad) OK\n";
    }

    // WHALESHARK2, 2026-10-02: an Engine-only third copy of a track, with
    // its file on the stick, matched OneLibrary's row for a different copy
    // by title and artist, and the sync wrote one file's cues onto the
    // other. A row whose file is present is that file: no name fallback.
    {
        Track engineOnly = makeTrack("e462", "10_song.mp3", 349.0, {CuePoint{CuePoint::Kind::Hot, 1, 30251.0, "", ""}},
                                     "Too Little Too Late (feat. Underworld)", "Joris Voorn, Underworld");
        engineOnly.filePath = "/stick/Contents/10_song.mp3";
        engineOnly.fileSizeBytes = 9'000'000;
        Track other = makeTrack("ol302", "28_song.mp3", 349.0, {}, "Too Little Too Late (feat. Underworld)",
                                "Joris Voorn, Underworld");
        other.filePath = "/stick/Contents/28_song.mp3";
        other.fileSizeBytes = 9'100'000;
        assert(TrackMatcher::match({engineOnly}, {other}).empty() && "a present file is never matched by name");

        // The fallback stays for a row that cannot be found by its file:
        // the committed fixture has catalogs and no audio.
        Track noAudio = engineOnly;
        noAudio.fileSizeBytes = 0;
        assert(TrackMatcher::match({noAudio}, {other}).size() == 1 && "no file behind the path: matched by name");
        std::cout << "case (a row whose file is on the stick is not matched to another file by name) OK\n";

        // The same two rows across libraries are the same song under two
        // paths, and must match: a track stored from one stick against
        // another stick's copy is what Metadata Backup and Restore do, and
        // is the matcher's default. afa3dbdb applied the one-stick rule
        // here too and halved what Restore could match.
        assert(matchTracks({engineOnly}, {other}).size() == 1 && "across libraries a name still matches");
        assert(matchTracks({engineOnly}, {other}, MatchScope::OneStick).empty());
        std::cout << "case (across libraries the same rows match by name) OK\n";
    }

    // ---- Every conflict says why (2026-10-04) -------------------------
    const auto hot = [](int pad, double ms) { return CuePoint{CuePoint::Kind::Hot, pad, ms, "", ""}; };
    const auto memory = [](double ms) { return CuePoint{CuePoint::Kind::Memory, 0, ms, "", ""}; };
    const auto side = [](const char *format, double bpm, std::vector<CuePoint> cues) {
        Track t = makeTrack(std::string(format) + "1", "song.mp3", 300.0, std::move(cues));
        t.format = format;
        t.bpm = bpm;
        return t;
    };

    // The tolerance is half a beat: 234 ms at 128 BPM. 230 ms apart is one
    // cue on one beat; 240 ms is nearer the next beat, and the DJ is shown
    // the offset.
    {
        auto same = SyncPlanner::plan(SyncMatch{side("rekordbox", 128.0, {hot(1, 30000.0)}),
                                                side("engine", 128.0, {hot(1, 30230.0)})},
                                      now, now);
        assert(same.kind == SyncPlan::Kind::AlreadyConsistent && "230 ms at 128 BPM is one cue");
        assert(std::abs(same.positionToleranceMs - 30000.0 / 128.0) < 1e-9);
        auto apart = SyncPlanner::plan(SyncMatch{side("rekordbox", 128.0, {hot(1, 30000.0)}),
                                                 side("engine", 128.0, {hot(1, 30240.0)})},
                                       now, now);
        assert(apart.needsChoice && apart.reason == SyncPlan::Reason::SamePadApart);
        assert(apart.reasonText
               == "Pad 1 is 240 ms apart (more than half a beat at 128 BPM): rekordbox 0:30.000, Engine 0:30.240");
        // The brief's own example, at 124 BPM.
        auto example = SyncPlanner::plan(SyncMatch{side("rekordbox", 124.0, {hot(1, 30765.0)}),
                                                   side("engine", 124.0, {hot(1, 30251.0)})},
                                         now, now);
        assert(example.reasonText
               == "Pad 1 is 514 ms apart (more than half a beat at 124 BPM): rekordbox 0:30.765, Engine 0:30.251");
        std::cout << "case (230 ms at 128 BPM is the same cue, 240 ms is a choice that shows the offset) OK\n";
    }

    // Without a tempo both sides agree on, the fallback is 60 ms, which
    // still holds every measured rounding: 1 ms (Engine's sample offsets)
    // and 52 ms (two MP3 frames of decoder delay) are the same cue. 60 ms
    // is two cues there, but half a beat of either tempo would have made
    // it one, so it is a choice that says why.
    {
        for (const double offset : {1.0, 52.0}) {
            auto plan = SyncPlanner::plan(SyncMatch{side("rekordbox", 128.0, {hot(1, 30000.0)}),
                                                    side("engine", 126.0, {hot(1, 30000.0 + offset)})},
                                          now, now);
            assert(plan.kind == SyncPlan::Kind::AlreadyConsistent && "a measured rounding is the same cue");
            assert(plan.positionToleranceMs == CueFallbackToleranceMs);
        }
        auto sixty = SyncPlanner::plan(SyncMatch{side("rekordbox", 128.0, {hot(1, 30000.0)}),
                                                 side("engine", 126.0, {hot(1, 30060.0)})},
                                       now, now);
        assert(sixty.kind == SyncPlan::Kind::Conflict && sixty.needsChoice);
        assert(sixty.reason == SyncPlan::Reason::TempoUnsure);
        // The pad and the distance stay in front of the tempo's reason.
        assert(sixty.reasonText
               == "Pad 1 is 60 ms apart: rekordbox 0:30.000, Engine 0:30.060. Tempos differ (128.0 vs 126.0), so cues "
                  "within half a beat cannot be matched; 60 ms was used");
        assert(sixty.direction == SyncPlan::Direction::None && !sixty.cuesIfAWins.empty() && !sixty.cuesIfBWins.empty());

        auto noTempo = SyncPlanner::plan(SyncMatch{side("rekordbox", 0.0, {hot(1, 30000.0)}),
                                                   side("engine", 126.0, {hot(1, 30100.0)})},
                                         now, now);
        assert(noTempo.reason == SyncPlan::Reason::TempoUnsure);
        assert(noTempo.reasonText
               == "Pad 1 is 100 ms apart: rekordbox 0:30.000, Engine 0:30.100. rekordbox has no tempo for this track, "
                  "so cues within half a beat cannot be matched; 60 ms was used");
        auto neither = SyncPlanner::plan(SyncMatch{side("rekordbox", 0.0, {hot(1, 30000.0)}),
                                                   side("engine", 0.0, {hot(1, 30100.0)})},
                                         now, now);
        assert(neither.reasonText
               == "Pad 1 is 100 ms apart: rekordbox 0:30.000, Engine 0:30.100. Neither side has a tempo for this "
                  "track, so cues within half a beat cannot be matched; 60 ms was used");
        // A tempo no track has is no tempo: two sides "agreeing" on 1 BPM
        // would make half a beat 30 seconds.
        auto garbage = SyncPlanner::plan(SyncMatch{side("rekordbox", 1.0, {hot(1, 30000.0)}),
                                                   side("engine", 1.0, {hot(1, 40000.0)})},
                                         now, now);
        assert(garbage.positionToleranceMs == CueFallbackToleranceMs && garbage.needsChoice);
        // And it is called out of range, not missing.
        auto fast = SyncPlanner::plan(SyncMatch{side("rekordbox", 320.0, {hot(1, 30000.0)}),
                                                side("engine", 0.0, {hot(1, 30100.0)})},
                                      now, now);
        assert(fast.reasonText
               == "Pad 1 is 100 ms apart: rekordbox 0:30.000, Engine 0:30.100. rekordbox's tempo (320.0) is out of "
                  "range and Engine has no tempo for this track, so cues within half a beat cannot be matched; 60 ms "
                  "was used");

        // Where no half beat could have mattered the reason is the
        // difference itself, not the tempo: 700 ms is more than half a
        // beat at the one tempo named, and a pad 4 s away is another cue.
        auto far = SyncPlanner::plan(SyncMatch{side("rekordbox", 128.0, {hot(1, 30000.0)}),
                                               side("engine", 0.0, {hot(1, 30700.0)})},
                                     now, now);
        assert(far.reason == SyncPlan::Reason::SamePadApart);
        assert(far.reasonText == "Pad 1 is 700 ms apart: rekordbox 0:30.000, Engine 0:30.700");
        auto other = SyncPlanner::plan(SyncMatch{side("rekordbox", 128.0, {hot(1, 30000.0)}),
                                                 side("engine", 126.0, {hot(1, 34000.0)})},
                                       now, now);
        assert(other.reason == SyncPlan::Reason::PadsDiffer);
        std::cout << "case (no agreed tempo: 1 and 52 ms are one cue, 60 ms is a choice saying why) OK\n";
    }

    // Pads differ: one line naming the pads, three at most, then a count.
    {
        auto plan = SyncPlanner::plan(
            SyncMatch{side("rekordbox", 124.0, {hot(1, 10000.0), hot(2, 20000.0), hot(3, 67751.0), hot(4, 90000.0)}),
                      side("engine", 124.0, {hot(1, 15000.0), hot(3, 30251.0), hot(4, 95000.0), hot(5, 120000.0)})},
            now, now);
        assert(plan.reason == SyncPlan::Reason::PadsDiffer);
        assert(plan.reasonText
               == "Pad 1: rekordbox 0:10.000, Engine 0:15.000; Pad 2: rekordbox 0:20.000, Engine empty; "
                  "Pad 3: rekordbox 1:07.751, Engine 0:30.251; and 2 more pads differ");
        auto one = SyncPlanner::plan(SyncMatch{side("rekordbox", 124.0, {hot(3, 67751.0)}),
                                               side("engine", 124.0, {hot(3, 30251.0)})},
                                     now, now);
        assert(one.reasonText == "Pad 3: rekordbox 1:07.751, Engine 0:30.251");
        std::cout << "case (pads that differ are named, with both positions) OK\n";
    }

    // Loop vs cue: the same pad and place, a loop on one side. It used to
    // read as the same cue.
    {
        CuePoint loop = hot(2, 30000.0);
        loop.isLoop = true;
        loop.loopEndMs = 37742.0;
        auto plan = SyncPlanner::plan(SyncMatch{side("rekordbox", 124.0, {hot(1, 1000.0), hot(2, 30000.0)}),
                                                side("engine", 124.0, {hot(1, 1000.0), loop})},
                                      now, now);
        assert(plan.needsChoice && plan.reason == SyncPlan::Reason::LoopVsCue);
        assert(plan.reasonText == "Pad 2 is a loop on Engine and a cue on rekordbox (0:30.000)");

        // WHALESHARK2, "Roam Zwei": Engine's pad 1 holds a cue and a loop,
        // OneLibrary's only the cue, 52 ms off. The cue is the same cue;
        // what differs is the loop, and the reason says so rather than
        // showing two cues 52 ms apart.
        CuePoint engineLoop = hot(1, 23051.0);
        engineLoop.isLoop = true;
        auto roam = SyncPlanner::plan(
            SyncMatch{side("engine", 132.0, {hot(1, 49486.0), engineLoop, hot(2, 56545.0)}),
                      side("onelibrary", 132.0, {hot(1, 49538.0), hot(2, 56545.0)})},
            now, now);
        assert(roam.needsChoice && roam.reason == SyncPlan::Reason::PadsDiffer);
        assert(roam.reasonText == "Pad 1: Engine loop 0:23.051, OneLibrary no loop");

        // A loop and a cue at different starts name both.
        CuePoint lateLoop = hot(2, 30040.0);
        lateLoop.isLoop = true;
        auto both = SyncPlanner::plan(SyncMatch{side("rekordbox", 124.0, {hot(2, 30000.0)}),
                                                side("engine", 124.0, {lateLoop})},
                                      now, now);
        assert(both.reasonText == "Pad 2 is a loop on Engine and a cue on rekordbox (rekordbox 0:30.000, Engine 0:30.040)");

        // A second cue on a pad is compared too, not hidden behind the
        // first: two cues on pad 1 against one is a difference.
        auto doubled = SyncPlanner::plan(SyncMatch{side("rekordbox", 124.0, {hot(1, 10000.0), hot(1, 40000.0)}),
                                                   side("onelibrary", 124.0, {hot(1, 10000.0)})},
                                         now, now);
        assert(doubled.kind == SyncPlan::Kind::Conflict && doubled.needsChoice);
        assert(doubled.reasonText == "Pad 1: rekordbox 0:40.000, OneLibrary no cue");
        // The cue named is the one without a counterpart, not the one that
        // shares its index with the other side's.
        auto extra = SyncPlanner::plan(SyncMatch{side("rekordbox", 124.0, {hot(1, 10000.0), hot(1, 40000.0)}),
                                                 side("onelibrary", 124.0, {hot(1, 40000.0)})},
                                       now, now);
        assert(extra.reasonText == "Pad 1: rekordbox 0:10.000, OneLibrary no cue");

        // A memory loop is not the memory cue at its start: the side
        // lacking the loop receives it, and one write settles the pair.
        CuePoint memoryLoop = memory(10000.0);
        memoryLoop.isLoop = true;
        Track withLoop = side("rekordbox", 124.0, {hot(1, 1000.0), memory(10000.0), memoryLoop});
        Track withoutLoop = side("onelibrary", 124.0, {hot(1, 1000.0), memory(10000.0)});
        auto gainsLoop = SyncPlanner::plan(SyncMatch{withLoop, withoutLoop}, now, now);
        assert(gainsLoop.direction == SyncPlan::Direction::ToB);
        withoutLoop.cues = gainsLoop.cuesToApply;
        assert(SyncPlanner::plan(SyncMatch{withLoop, withoutLoop}, now, now).kind == SyncPlan::Kind::AlreadyConsistent
               && "the write carried the loop; no write that changes nothing");

        // Two memory cues half a beat apart are two cues between catalogs
        // that both hold memory cues: the side with one gains the other.
        Track two = side("rekordbox", 128.0, {hot(1, 1000.0), memory(60000.0), memory(60100.0)});
        Track one = side("onelibrary", 128.0, {hot(1, 1000.0), memory(60000.0)});
        auto gainsSecond = SyncPlanner::plan(SyncMatch{two, one}, now, now);
        assert(gainsSecond.direction == SyncPlan::Direction::ToB && "the side with one is written, not the one with both");
        one.cues = gainsSecond.cuesToApply;
        assert(SyncPlanner::plan(SyncMatch{two, one}, now, now).kind == SyncPlan::Kind::AlreadyConsistent);
        std::cout << "case (a loop against a cue on one pad is a choice) OK\n";
    }

    // A loop's out point is compared too (#60 follow-up). The same loop
    // on one pad, its end moved on one side by more than half a beat: its
    // length changed, nothing says which side is right, and it is a choice
    // with its own reason. It used to be neither synced nor flagged.
    {
        const auto loopOn = [&](int pad, double start, double end) {
            CuePoint cue = hot(pad, start);
            cue.isLoop = true;
            cue.loopEndMs = end;
            return cue;
        };
        auto plan = SyncPlanner::plan(SyncMatch{side("rekordbox", 124.0, {hot(1, 1000.0), loopOn(2, 15000.0, 23265.0)}),
                                                side("engine", 124.0, {hot(1, 1000.0), loopOn(2, 15000.0, 24000.0)})},
                                      now, now);
        assert(plan.kind == SyncPlan::Kind::Conflict && plan.needsChoice);
        assert(plan.reason == SyncPlan::Reason::LoopEndsDiffer);
        assert(plan.reasonText == "Pad 2 loop ends differ: rekordbox 0:23.265, Engine 0:24.000");
        assert(plan.direction == SyncPlan::Direction::None && plan.cuesToApply.empty());

        // In the pair's own order, Engine first, and between two catalogs
        // that both hold pads.
        auto engineFirst = SyncPlanner::plan(SyncMatch{side("engine", 124.0, {loopOn(2, 15000.0, 24000.0)}),
                                                       side("rekordbox", 124.0, {loopOn(2, 15000.0, 23265.0)})},
                                             now, now);
        assert(engineFirst.reasonText == "Pad 2 loop ends differ: Engine 0:24.000, rekordbox 0:23.265");
        auto twoCatalogs = SyncPlanner::plan(SyncMatch{side("rekordbox", 124.0, {loopOn(2, 15000.0, 23265.0)}),
                                                       side("onelibrary", 124.0, {loopOn(2, 15000.0, 24000.0)})},
                                             now, now);
        assert(twoCatalogs.reason == SyncPlan::Reason::LoopEndsDiffer);
        assert(twoCatalogs.reasonText == "Pad 2 loop ends differ: rekordbox 0:23.265, OneLibrary 0:24.000");

        // Within half a beat (242 ms at 124 BPM) it is the same loop.
        auto rounded = SyncPlanner::plan(SyncMatch{side("rekordbox", 124.0, {loopOn(2, 15000.0, 23265.0)}),
                                                   side("engine", 124.0, {loopOn(2, 15001.0, 23317.0)})},
                                         now, now);
        assert(rounded.kind == SyncPlan::Kind::AlreadyConsistent);

        // A memory loop between two catalogs that hold memory cues: named,
        // not "changed on both sides".
        CuePoint memoryLoop = memory(10000.0);
        memoryLoop.isLoop = true;
        memoryLoop.loopEndMs = 14000.0;
        CuePoint longerMemoryLoop = memoryLoop;
        longerMemoryLoop.loopEndMs = 18000.0;
        auto memoryPlan = SyncPlanner::plan(SyncMatch{side("rekordbox", 124.0, {hot(1, 1000.0), memoryLoop}),
                                                      side("onelibrary", 124.0, {hot(1, 1000.0), longerMemoryLoop})},
                                            now, now);
        assert(memoryPlan.needsChoice && memoryPlan.reason == SyncPlan::Reason::LoopEndsDiffer);
        assert(memoryPlan.reasonText == "Memory loop at 0:10.000 ends differ: rekordbox 0:14.000, OneLibrary 0:18.000");

        // Engine holds that memory loop as a saved loop on the pad its
        // import gives it. Its end moved there: a choice, and each option
        // carries its own side's end.
        auto enginePlan = SyncPlanner::plan(SyncMatch{side("rekordbox", 124.0, {memoryLoop}),
                                                      side("engine", 124.0, {loopOn(1, 10000.0, 18000.0)})},
                                            now, now);
        assert(enginePlan.needsChoice && enginePlan.reason == SyncPlan::Reason::LoopEndsDiffer);
        assert(enginePlan.reasonText == "Pad 1 loop ends differ: rekordbox 0:14.000, Engine 0:18.000");
        const auto endOf = [](const std::vector<CuePoint> &cues) {
            for (const CuePoint &cue : cues) {
                if (cue.isLoop) {
                    return cue.loopEndMs;
                }
            }
            return -1.0;
        };
        assert(endOf(enginePlan.cuesIfAWins) == 14000.0 && "rekordbox's way writes its end onto Engine's pad");
        assert(endOf(enginePlan.cuesIfBWins) == 18000.0 && "Engine's way gives rekordbox's memory loop Engine's end");
        // No string literal of a reason holds a dash (the UI's rule).
        for (const auto *text : {&plan.reasonText, &memoryPlan.reasonText, &enginePlan.reasonText}) {
            assert(text->find(std::string(2, '-')) == std::string::npos && text->find("\xE2\x80\x94") == std::string::npos);
        }
        std::cout << "case (a loop whose out point moved is a choice, with its reason) OK\n";
    }

    // Both sides changed: each has a memory cue the other lacks. An
    // addition here or a removal there; only a clock could say, and none
    // is asked, whichever way the times point.
    for (const bool aIn2017 : {true, false}) {
        Track a = side("rekordbox", 124.0, {hot(1, 1000.0), memory(10000.0), memory(20000.0)});
        Track b = side("onelibrary", 124.0, {hot(1, 1000.0), memory(10000.0), memory(30000.0)});
        a.metadataModifiedAt = aIn2017 ? 1'500'000'000 : 1'790'000'000;
        b.metadataModifiedAt = aIn2017 ? 1'790'000'000 : 1'500'000'000;
        auto plan = aIn2017 ? SyncPlanner::plan(SyncMatch{a, b}, now - hours(24 * 365 * 9), now)
                            : SyncPlanner::plan(SyncMatch{a, b}, now, now - hours(24 * 365 * 9));
        assert(plan.kind == SyncPlan::Kind::Conflict && plan.needsChoice);
        assert(plan.direction == SyncPlan::Direction::None && plan.cuesToApply.empty());
        assert(plan.reason == SyncPlan::Reason::BothChanged);
        assert(plan.reasonText == "Changed on both sides; the stick's clocks cannot say which is newer");
        // A side that only lacks something still just receives it.
        Track c = side("onelibrary", 124.0, {hot(1, 1000.0), memory(10000.0)});
        auto gains = SyncPlanner::plan(SyncMatch{a, c}, now, now);
        assert(!gains.needsChoice && gains.direction == SyncPlan::Direction::ToB);
    }
    std::cout << "case (memory cues changed on both sides are a choice, whatever the clocks say) OK\n";

    // Engine pads at rekordbox's memory cues. Engine DJ's older import put
    // them on the free pads in time order, and on WHALESHARK2 shifted MP3s
    // by 52 ms ("Desire"): that measured shape is the memory cues, in sync.
    {
        Track r = side("rekordbox", 124.0, {hot(1, 1000.0), hot(2, 20000.0), memory(60000.0), memory(90000.0)});
        Track e = side("engine", 124.0, {hot(1, 1052.0), hot(2, 20052.0), hot(3, 60052.0), hot(4, 90052.0),
                                         memory(60052.0)});
        auto plan = SyncPlanner::plan(SyncMatch{r, e}, now, now);
        assert(plan.kind == SyncPlan::Kind::AlreadyConsistent && "the import's own pads, 52 ms late, are in sync");

        // The same pads swapped: not what the import does. A translation,
        // or two hot cues the DJ set there? Asked.
        Track swapped = side("engine", 124.0, {hot(1, 1000.0), hot(2, 20000.0), hot(4, 60000.0), hot(3, 90000.0),
                                               memory(60000.0)});
        auto asked = SyncPlanner::plan(SyncMatch{r, swapped}, now, now);
        assert(asked.needsChoice && asked.reason == SyncPlan::Reason::EngineMemoryOrHotCue);
        assert(asked.reasonText
               == "Engine pads 4 and 3 sit where rekordbox has memory cues (1:00.000, 1:30.000); "
                  "translated memory cues or new hot cues?");

        // An Engine pad at a memory cue on a pad rekordbox's own hot cue
        // holds elsewhere: the brief's case.
        Track r2 = side("rekordbox", 123.0, {hot(1, 1000.0), hot(2, 140000.0), memory(9904.0)});
        Track e2 = side("engine", 123.0, {hot(1, 1000.0), hot(2, 9904.0), memory(9904.0)});
        auto pad2 = SyncPlanner::plan(SyncMatch{r2, e2}, now, now);
        assert(pad2.needsChoice && pad2.reason == SyncPlan::Reason::EngineMemoryOrHotCue);
        assert(pad2.reasonText
               == "Engine pad 2 sits where rekordbox has a memory cue (0:09.904); a translated memory cue or a new "
                  "hot cue?");
        // Either answer settles it: rekordbox's puts the memory cue on the
        // import's pad and pad 2 back; Engine's makes pad 2 a hot cue on
        // rekordbox too, and the next sync is a plain copy.
        Track e2After = e2;
        e2After.cues = pad2.cuesIfAWins;
        assert(SyncPlanner::plan(SyncMatch{r2, e2After}, now, now).kind == SyncPlan::Kind::AlreadyConsistent);
        Track r2After = r2;
        r2After.cues = pad2.cuesIfBWins;
        auto next = SyncPlanner::plan(SyncMatch{r2After, e2}, now, now);
        assert(!next.needsChoice);
        if (next.direction != SyncPlan::Direction::None) {
            e2.cues = next.cuesToApply;
        }
        assert(SyncPlanner::plan(SyncMatch{r2After, e2}, now, now).kind == SyncPlan::Kind::AlreadyConsistent);

        // Two memory cues closer than half a beat: the import gives the
        // earlier one pad 1 (and the later none, as "on a pad already").
        // Engine's pad 1 between them is that translation, whichever of
        // the two rekordbox lists first; nothing to ask.
        Track close = side("rekordbox", 128.0, {memory(60150.0), memory(60000.0)});
        Track closeE = side("engine", 128.0, {hot(1, 60100.0), memory(60000.0)});
        assert(SyncPlanner::plan(SyncMatch{close, closeE}, now, now).kind == SyncPlan::Kind::AlreadyConsistent);

        // A second pad for the marker under hot cue 3 ("Voices In My Head"
        // with the duplicate the old translation made): asked, not taken.
        Track voicesR = side("rekordbox", 124.0, {hot(3, 52583.0), memory(52583.0)});
        Track voicesE = side("engine", 124.0, {hot(3, 52583.0), hot(6, 52583.0), memory(52583.0)});
        auto voices = SyncPlanner::plan(SyncMatch{voicesR, voicesE}, now, now);
        assert(voices.needsChoice && voices.reason == SyncPlan::Reason::EngineMemoryOrHotCue);
        std::cout << "case (Engine pads at memory cues: the import's shape is in sync, any other is asked) OK\n";
    }

    // Cues Engine has no pad for are said, not silent.
    {
        std::vector<CuePoint> cues;
        for (int pad = 1; pad <= 8; ++pad) {
            cues.push_back(hot(pad, pad * 10000.0));
        }
        cues.push_back(memory(100000.0));
        cues.push_back(memory(130500.0));
        auto plan = SyncPlanner::plan(SyncMatch{side("rekordbox", 124.0, cues), side("engine", 124.0, {})}, now, now);
        assert(plan.cuesLeftOut.size() == 2);
        assert(describeCuesLeftOut(plan.cuesLeftOut)
               == "2 cues stay off Engine: its eight pads are full (1:40.000, 2:10.500)");
        assert(describeCuesLeftOut({}).empty());
        std::cout << "case (cues left off Engine are described) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
