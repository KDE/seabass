// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include "domain/sync_planning.hpp"

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

    // Conflict resolved by mtime: rekordbox file newer -> wins, propagates to engine.
    {
        SyncMatch m{makeTrack("r1", "song.mp3", 200.0,
                              {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"}}),
                    makeTrack("e1", "song.mp3", 200.0,
                              {CuePoint{CuePoint::Kind::Hot, 1, 5000.0, "#00FF00", "intro"}})};
        auto plan = SyncPlanner::plan(m, now, now - hours(1));
        assert(plan.kind == SyncPlan::Kind::Conflict);
        assert(plan.direction == SyncPlan::Direction::ToB);
        std::cout << "case 6 (conflict, rekordbox newer -> to engine) OK\n";
    }

    // Conflict resolved by mtime: engine file newer -> wins, propagates to rekordbox.
    {
        SyncMatch m{makeTrack("r1", "song.mp3", 200.0,
                              {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"}}),
                    makeTrack("e1", "song.mp3", 200.0,
                              {CuePoint{CuePoint::Kind::Hot, 1, 5000.0, "#00FF00", "intro"}})};
        auto plan = SyncPlanner::plan(m, now - hours(1), now);
        assert(plan.kind == SyncPlan::Kind::Conflict);
        assert(plan.direction == SyncPlan::Direction::ToA);
        std::cout << "case 7 (conflict, engine newer -> to rekordbox) OK\n";
    }

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
        assert(plan.kind == SyncPlan::Kind::AlreadyConsistent);
        assert(plan.direction == SyncPlan::Direction::None);
        std::cout << "case 8 (Engine's one memory cue among rekordbox's three is agreement, not a wipe) OK\n";
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
        assert(plan.direction == SyncPlan::Direction::ToA);
        int hot = 0;
        int memory = 0;
        for (const auto &cue : plan.cuesToApply) {
            if (cue.kind == CuePoint::Kind::Hot) {
                hot++;
                assert(cue.positionMs == 9000.0);  // the newer side's hot cue
            } else {
                memory++;
            }
        }
        assert(hot == 1);
        assert(memory == 3);  // none of rekordbox's memory cues is taken away
        std::cout << "case 9 (a hot cue conflict keeps every memory cue) OK\n";
    }

    // Per-track clocks outrank the catalog files. The catalogs say Engine is
    // newer (m.db was written an hour ago for some other track), but this
    // track was last edited on the rekordbox side.
    {
        Track r = makeTrack("r1", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""}});
        r.format = "rekordbox";
        r.metadataModifiedAt = 1'800'000'000;
        Track e = makeTrack("e1", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 5000.0, "#FF0000", ""}});
        e.format = "engine";
        e.metadataModifiedAt = 1'700'000'000;
        auto plan = SyncPlanner::plan(SyncMatch{r, e}, now - hours(1), now);
        assert(plan.kind == SyncPlan::Kind::Conflict);
        assert(plan.direction == SyncPlan::Direction::ToB);
        std::cout << "case 10 (each track's own edit time outranks the catalog file's) OK\n";
    }

    // A side that cannot date its own track falls back to the catalog files
    // for both, rather than comparing a real date against zero.
    {
        Track r = makeTrack("r1", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""}});
        r.format = "rekordbox";
        r.metadataModifiedAt = 1'800'000'000;
        Track e = makeTrack("e1", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 5000.0, "#FF0000", ""}});
        e.format = "engine";
        e.metadataModifiedAt = 0;  // unknown
        auto plan = SyncPlanner::plan(SyncMatch{r, e}, now - hours(1), now);
        assert(plan.direction == SyncPlan::Direction::ToA);  // catalogs: Engine newer
        std::cout << "case 11 (an undatable track falls back to the catalog files) OK\n";
    }

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
        assert(plan.direction == SyncPlan::Direction::ToB);  // onto Engine: rekordbox's hot cue is newer
        std::vector<double> memory;
        for (const auto &cue : plan.cuesToApply) {
            if (cue.kind == CuePoint::Kind::Hot) {
                assert(cue.positionMs == 1000.0);
            } else {
                memory.push_back(cue.positionMs);
            }
        }
        assert(memory.size() == 1 && memory[0] == 15000.0 && "Engine keeps its own memory cue");
        std::cout << "case 12 (a hot cue sync onto Engine keeps Engine's only memory cue) OK\n";
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

        // And after that write the pair is consistent: Engine's one memory
        // cue is among rekordbox's three.
        r.cues = plan.cuesToApply;
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
        assert(plan.hotCuesNeedChoice && "both sides have different hot cues: the DJ chooses");
        // Both choices are ready to write, each with its own hot cues.
        assert(!plan.cuesIfAWins.empty() && plan.cuesIfAWins.front().positionMs == 1000.0);
        assert(!plan.cuesIfBWins.empty() && plan.cuesIfBWins.front().positionMs == 5000.0);
        // The suggestion is still the newer side, for display only.
        assert(plan.direction == SyncPlan::Direction::ToA);
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
        assert(!plan.hotCuesNeedChoice);
        assert(plan.direction == SyncPlan::Direction::ToB && "the side with hot cues wins; none are erased");
        std::cout << "case 16 (hot cues on one side only are copied across without asking) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
