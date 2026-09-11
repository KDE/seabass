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

    std::cout << "all cases passed\n";
    return 0;
}
