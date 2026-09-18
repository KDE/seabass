// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include "domain/junk_cue.hpp"

using namespace seabass::domain;

namespace
{

CuePoint makeCue(CuePoint::Kind kind, double positionMs)
{
    CuePoint c;
    c.kind = kind;
    c.positionMs = positionMs;
    return c;
}

Track makeTrack(std::string id, std::string title, std::string artist, std::vector<CuePoint> cues)
{
    Track t;
    t.sourceId = std::move(id);
    t.title = std::move(title);
    t.artist = std::move(artist);
    t.cues = std::move(cues);
    return t;
}

}  // namespace

int main()
{
    // Case 1: a memory cue at 0:00 is junk.
    {
        std::vector<Track> tracks = {
            makeTrack("a", "Song", "Artist", {makeCue(CuePoint::Kind::Memory, 0.0)}),
        };
        auto issues = JunkCueFinder::find(tracks);
        assert(issues.size() == 1);
        assert(issues[0].track.sourceId == "a");
        assert(issues[0].cue.kind == CuePoint::Kind::Memory);
        std::cout << "case 1 (memory cue at 0:00 is flagged) OK\n";
    }

    // Case 2: a hot cue at 0:00 is junk as well.
    //
    // It was exempt on the theory that some DJs keep a deliberate
    // track-start pad there. Real sticks have one or two to a library, at
    // 7 ms and 109 ms, sitting among the memory-cue noise and no more use
    // to navigate by: the track already starts at 0:00.
    {
        std::vector<Track> tracks = {
            makeTrack("a", "Song", "Artist", {makeCue(CuePoint::Kind::Hot, 0.0)}),
        };
        auto issues = JunkCueFinder::find(tracks);
        assert(issues.size() == 1);
        assert(issues[0].cue.kind == CuePoint::Kind::Hot);
        std::cout << "case 2 (a hot cue at 0:00 is flagged too) OK\n";
    }

    // Case 3: a memory cue away from 0:00 is not junk.
    {
        std::vector<Track> tracks = {
            makeTrack("a", "Song", "Artist", {makeCue(CuePoint::Kind::Memory, 12345.0)}),
        };
        auto issues = JunkCueFinder::find(tracks);
        assert(issues.empty());
        std::cout << "case 3 (memory cue away from 0:00 is not flagged) OK\n";
    }

    // Case 3b: a memory cue near, but not exactly at, 0:00 is still junk --
    // real Engine data has main_cue land a few hundred ms in (its own
    // analysis picks the first detected beat/transient, not sample 0),
    // and every mm:ss display in this app floors to "0:00" for anything
    // under a second, so the two must agree on what "at 0:00" means.
    {
        std::vector<Track> tracks = {
            makeTrack("a", "Song", "Artist", {makeCue(CuePoint::Kind::Memory, 539.0)}),
        };
        auto issues = JunkCueFinder::find(tracks);
        assert(issues.size() == 1);
        std::cout << "case 3b (memory cue at 539ms, displays as 0:00, is flagged) OK\n";
    }

    // Case 3c: a memory cue right at the 1-second boundary is not junk --
    // it displays as "0:01", genuinely a different, deliberate position.
    {
        std::vector<Track> tracks = {
            makeTrack("a", "Song", "Artist", {makeCue(CuePoint::Kind::Memory, 1000.0)}),
        };
        auto issues = JunkCueFinder::find(tracks);
        assert(issues.empty());
        std::cout << "case 3c (memory cue at exactly 1000ms, displays as 0:01, is not flagged) OK\n";
    }

    // Case 4: multiple tracks, multiple cues, only the matching ones surface.
    {
        std::vector<Track> tracks = {
            makeTrack("a", "Song A", "Artist", {makeCue(CuePoint::Kind::Memory, 0.0),
                                                 makeCue(CuePoint::Kind::Memory, 5000.0)}),
            makeTrack("b", "Song B", "Artist", {makeCue(CuePoint::Kind::Hot, 0.0)}),
            makeTrack("c", "Song C", "Artist", {makeCue(CuePoint::Kind::Memory, 0.0)}),
        };
        auto issues = JunkCueFinder::find(tracks);
        assert(issues.size() == 3);
        assert(issues[0].track.sourceId == "a");
        assert(issues[1].track.sourceId == "b" && "the hot cue at the start counts now too");
        assert(issues[2].track.sourceId == "c");
        std::cout << "case 4 (multiple tracks/cues, only true matches surface) OK\n";
    }

    // Case 5: a memory cue BEFORE the start. Seabass wrote these itself
    // -- Engine stores -1 as "no main cue set", and reading that as a
    // sample offset put a cue at minus a fraction of a millisecond.
    // Sticks and metadata backups made before that still carry them, so
    // the rule has to name them even though the reader no longer makes
    // them. The withoutJunkCues() filter is the same rule, and the
    // metadata paths lean on it.
    {
        const double sentinel = -1.0 / 44100.0 * 1000.0;  // what one real library was full of
        std::vector<Track> tracks = {
            makeTrack("a", "Song", "Artist",
                      {makeCue(CuePoint::Kind::Memory, sentinel), makeCue(CuePoint::Kind::Hot, 0.0),
                       makeCue(CuePoint::Kind::Memory, 30'000.0)}),
        };
        auto issues = JunkCueFinder::find(tracks);
        assert(issues.size() == 2 && "the sentinel and the hot cue at the start");
        assert(issues[0].cue.positionMs < 0.0);

        const auto kept = withoutJunkCues(tracks[0].cues);
        assert(kept.size() == 1);
        assert(kept[0].positionMs == 30'000.0);

        // A loop is the exception, wherever it starts: it has an end as
        // well as a start, which no stray press does.
        CuePoint introLoop = makeCue(CuePoint::Kind::Hot, 0.0);
        introLoop.isLoop = true;
        introLoop.loopEndMs = 8'000.0;
        assert(!isJunkCue(introLoop));
        std::cout << "case 5 (a cue before the start is junk; a loop never is) OK\n";
    }

    // Case 6: a hot cue in the first second is junk too.
    //
    // It counted as deliberate until real sticks were looked at: one or
    // two to a library, at 7 ms and 109 ms, indistinguishable from the
    // noise beside them and no use to navigate by, since the track
    // already starts there.
    {
        std::vector<Track> tracks = {
            makeTrack("a", "Song", "Artist",
                      {makeCue(CuePoint::Kind::Hot, 7.0), makeCue(CuePoint::Kind::Hot, 109.0),
                       makeCue(CuePoint::Kind::Hot, 30'000.0), makeCue(CuePoint::Kind::Memory, 0.0)}),
        };
        auto issues = JunkCueFinder::find(tracks);
        assert(issues.size() == 3 && "both hot cues at the start, and the memory cue");
        const auto kept = withoutJunkCues(tracks[0].cues);
        assert(kept.size() == 1);
        assert(kept[0].kind == CuePoint::Kind::Hot && kept[0].positionMs == 30'000.0);
        // The line is the same second for both kinds.
        assert(!isJunkCue(makeCue(CuePoint::Kind::Hot, 1000.0)));
        assert(isJunkCue(makeCue(CuePoint::Kind::Hot, 999.0)));
        std::cout << "case 6 (a hot cue in the first second is junk too) OK\n";
    }

    std::cout << "All junk_cue tests passed.\n";
    return 0;
}
