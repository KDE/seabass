// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
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
        std::vector<double> pads;
        for (const auto &cue : plan.cuesToApply) {
            if (cue.kind == CuePoint::Kind::Hot) {
                pads.push_back(cue.positionMs);
            } else {
                memory.push_back(cue.positionMs);
            }
        }
        assert((pads == std::vector<double>{1000.0, 10000.0, 20000.0}) && "rekordbox's hot cue, then its memory cues on pads");
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
                                         CuePoint{CuePoint::Kind::Memory, 0, 9400.0, "", ""}};
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
        assert(!plan.hotCuesNeedChoice);
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
        assert(!plan.hotCuesNeedChoice);
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
        assert(plan.hotCuesNeedChoice && "the slots really differ, which is a choice");
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

    std::cout << "all cases passed\n";
    return 0;
}
