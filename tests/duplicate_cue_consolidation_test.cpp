// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include "domain/duplicate_cue_consolidation.hpp"

using namespace seabass::domain;

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
    // Case 1: unambiguous -- one copy has cues, other doesn't.
    {
        std::vector<Track> tracks = {
            makeTrack("1", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"}}, "Song",
                      "Artist"),
            makeTrack("2", "song.mp3", 200.0, {}, "Song", "Artist"),
        };
        auto groups = DuplicateTrackFinder::find(tracks);
        assert(groups.size() == 1);
        auto plan = DuplicateCueConsolidator::plan(groups[0]);
        assert(plan.kind == ConsolidationPlan::Kind::Unambiguous);
        assert(plan.source->sourceId == "1");
        assert(plan.targets.size() == 1 && plan.targets[0].sourceId == "2");
        std::cout << "case 1 (unambiguous) OK\n";
    }

    // Case 2: conflict -- both have cues, but different.
    {
        std::vector<Track> tracks = {
            makeTrack("1", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"}}, "Song",
                      "Artist"),
            makeTrack("2", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 5000.0, "#00FF00", "intro"}}, "Song",
                      "Artist"),
        };
        auto groups = DuplicateTrackFinder::find(tracks);
        assert(groups.size() == 1);
        auto plan = DuplicateCueConsolidator::plan(groups[0]);
        assert(plan.kind == ConsolidationPlan::Kind::Conflict);
        std::cout << "case 2 (conflict) OK\n";
    }

    // Case 3: already consistent -- both have the same cues.
    {
        std::vector<Track> tracks = {
            makeTrack("1", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"}}, "Song",
                      "Artist"),
            makeTrack("2", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 1000.4, "#FF0000", "drop"}}, "Song",
                      "Artist"),
        };
        auto groups = DuplicateTrackFinder::find(tracks);
        assert(groups.size() == 1);
        auto plan = DuplicateCueConsolidator::plan(groups[0]);
        assert(plan.kind == ConsolidationPlan::Kind::AlreadyConsistent);
        std::cout << "case 3 (already consistent) OK\n";
    }

    // Case 4: an identical filename is NOT on its own a match. Export
    // numbering makes two copies of one song differ in name, and
    // truncation makes two different songs collide -- only artist +
    // title + length decide.
    {
        std::vector<Track> tracks = {
            makeTrack("1", "song.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"}}),
            makeTrack("2", "song.mp3", 200.0, {}),
        };
        auto groups = DuplicateTrackFinder::find(tracks);
        assert(groups.empty());
        std::cout << "case 4 (same filename, no metadata -> no group) OK\n";
    }

    // Case 5: same filename and same metadata, very different duration --
    // not clustered together.
    {
        std::vector<Track> tracks = {
            makeTrack("1", "song.mp3", 200.0, {}, "Song", "Artist"),
            makeTrack("2", "song.mp3", 9000.0, {}, "Song", "Artist"),
        };
        auto groups = DuplicateTrackFinder::find(tracks);
        assert(groups.empty());
        std::cout << "case 5 (duration mismatch -> no group) OK\n";
    }

    // Case 6: different filenames but matching title+artist -- grouped via
    // the title+artist path (e.g. re-imported under a new naming scheme).
    {
        std::vector<Track> tracks = {
            makeTrack("1", "099_ella-but_not_for_me.mp3", 200.0,
                      {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"}}, "But Not For Me",
                      "Ella Fitzgerald"),
            makeTrack("2", "03_ella-but_not_for_me.mp3", 200.0, {}, "But Not For Me", "Ella Fitzgerald"),
        };
        auto groups = DuplicateTrackFinder::find(tracks);
        assert(groups.size() == 1);
        assert(groups[0].tracks.size() == 2);
        auto plan = DuplicateCueConsolidator::plan(groups[0]);
        assert(plan.kind == ConsolidationPlan::Kind::Unambiguous);
        std::cout << "case 6 (different filename, same title+artist -> grouped) OK\n";
    }

    // Case 7: different filenames AND different title/artist -- no group.
    {
        std::vector<Track> tracks = {
            makeTrack("1", "a.mp3", 200.0, {}, "Song A", "Artist A"),
            makeTrack("2", "b.mp3", 200.0, {}, "Song B", "Artist B"),
        };
        auto groups = DuplicateTrackFinder::find(tracks);
        assert(groups.empty());
        std::cout << "case 7 (different everything -> no group) OK\n";
    }

    // Case 8: same title+artist, but durations too far apart -- title+artist
    // alone isn't enough; duration tolerance still applies (e.g. a short
    // radio edit misidentified under the same metadata as the full track).
    {
        std::vector<Track> tracks = {
            makeTrack("1", "a.mp3", 200.0, {}, "Song", "Artist"),
            makeTrack("2", "b.mp3", 9000.0, {}, "Song", "Artist"),
        };
        auto groups = DuplicateTrackFinder::find(tracks);
        assert(groups.empty());
        std::cout << "case 8 (same title+artist, duration mismatch -> no group) OK\n";
    }

    // Case 9: tracks with no title/artist metadata must never be merged with
    // each other just because they all share an empty key -- title+artist
    // matching is skipped entirely when either field is blank.
    {
        std::vector<Track> tracks = {
            makeTrack("1", "a.mp3", 200.0, {}),
            makeTrack("2", "b.mp3", 200.0, {}),
            makeTrack("3", "c.mp3", 200.0, {}),
        };
        auto groups = DuplicateTrackFinder::find(tracks);
        assert(groups.empty());
        std::cout << "case 9 (blank title/artist never false-matches) OK\n";
    }

    // Case 10: a filename match must not drag an unrelated track into a
    // real group. B and C are the same song by artist+title+length; A
    // only shares B's filename and has no metadata of its own, so it
    // stays out. Under the old filename-matching rule A was pulled in
    // transitively and would have been queued for deletion.
    {
        std::vector<Track> tracks = {
            makeTrack("A", "shared.mp3", 200.0, {}),
            makeTrack("B", "shared.mp3", 200.0, {}, "Song", "Artist"),
            makeTrack("C", "other.mp3", 200.0, {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"}}, "Song",
                      "Artist"),
        };
        auto groups = DuplicateTrackFinder::find(tracks);
        assert(groups.size() == 1);
        assert(groups[0].tracks.size() == 2);
        for (const auto &track : groups[0].tracks) {
            assert(track.sourceId != "A");
        }
        std::cout << "case 10 (filename alone never joins a group) OK\n";
    }

    // Case 11: a duration of 0 means "unreadable," not "really zero
    // seconds long" (matches Track's own fallback convention for other
    // fields) -- and an unreadable duration means "cannot confirm these
    // are the same recording", so the track is never grouped.
    //
    // This deliberately reverses the earlier rule, which grouped a pair
    // whenever either duration was missing so that a real cluster would
    // not be split by a failed reading. That was the wrong trade for a
    // destructive caller: on a real Engine stick 77.6% of rows had no
    // duration (Engine leaves `length` NULL until it analyzes a track),
    // and the permissive rule put a radio edit and an extended mix of
    // the same artist+title in one group 40 times, 39 of which the Clean
    // Up planner had checked for deletion. Not grouping costs a missed
    // duplicate; grouping wrongly costs unique audio.
    {
        std::vector<Track> tracks = {
            makeTrack("unknownA", "07_song.mp3", 0.0, {}, "Song", "Artist"),
            makeTrack("unknownB", "11_song_1.mp3", 0.0, {}, "Song", "Artist"),
            makeTrack("known", "11_song.mp3", 463.0, {}, "Song", "Artist"),
        };
        auto groups = DuplicateTrackFinder::find(tracks);
        assert(groups.empty());
        std::cout << "case 11 (unknown duration -> never grouped) OK\n";
    }

    // Case 12: two copies that agree on artist, title AND length are a
    // duplicate pair even though their filenames share nothing -- the
    // positive counterpart to case 11.
    {
        std::vector<Track> tracks = {
            makeTrack("1", "072_paul-no-goodbye.mp3", 390.9, {}, "No Goodbye", "Paul Kalkbrenner"),
            makeTrack("2", "30_Paul Kalkbrenner-No Goodbye.mp3", 390.4, {}, "No Goodbye", "Paul Kalkbrenner"),
        };
        auto groups = DuplicateTrackFinder::find(tracks);
        assert(groups.size() == 1);
        assert(groups[0].tracks.size() == 2);
        std::cout << "case 12 (artist+title+length agree -> grouped) OK\n";
    }

    // Case 13: the real regression this change exists for -- a 2:47 radio
    // edit and a 6:31 extended mix, same artist+title, both durations
    // known. Must never be one group.
    {
        std::vector<Track> tracks = {
            makeTrack("edit", "72_Paul Kalkbrenner-No Goodbye.mp3", 167.3, {}, "No Goodbye", "Paul Kalkbrenner"),
            makeTrack("extended", "30_Paul Kalkbrenner-No Goodbye.mp3", 390.9, {}, "No Goodbye", "Paul Kalkbrenner"),
        };
        auto groups = DuplicateTrackFinder::find(tracks);
        assert(groups.empty());
        std::cout << "case 13 (radio edit vs extended mix -> never grouped) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
