// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include "domain/track_matching.hpp"

using namespace seabass::domain;

int main()
{
    // normalizeFilename: case-insensitive, whitespace-insensitive.
    {
        assert(normalizeFilename("Song.mp3") == normalizeFilename("song.mp3"));
        assert(normalizeFilename("My Song.mp3") == normalizeFilename("MySong.mp3"));
        assert(normalizeFilename("a.mp3") != normalizeFilename("b.mp3"));
        std::cout << "case 1 (normalizeFilename case/whitespace insensitive) OK\n";
    }

    // cueSetsEqual: identical sets, in a different order, are still equal.
    {
        std::vector<CuePoint> a = {
            CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"},
            CuePoint{CuePoint::Kind::Hot, 2, 5000.0, "#00FF00", "break"},
        };
        std::vector<CuePoint> b = {
            CuePoint{CuePoint::Kind::Hot, 2, 5000.0, "#00FF00", "break"},
            CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"},
        };
        assert(cueSetsEqual(a, b));
        std::cout << "case 2 (cueSetsEqual ignores order) OK\n";
    }

    // cueSetsEqual: different sizes are never equal.
    {
        std::vector<CuePoint> a = {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""}};
        std::vector<CuePoint> b = {};
        assert(!cueSetsEqual(a, b));
        std::cout << "case 3 (cueSetsEqual size mismatch -> false) OK\n";
    }

    // cueSetsEqual: position tolerance is inclusive at 1000ms, exclusive past it.
    {
        std::vector<CuePoint> a = {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""}};
        std::vector<CuePoint> withinTolerance = {CuePoint{CuePoint::Kind::Hot, 1, 2000.0, "#FF0000", ""}};
        std::vector<CuePoint> pastTolerance = {CuePoint{CuePoint::Kind::Hot, 1, 2000.1, "#FF0000", ""}};
        assert(cueSetsEqual(a, withinTolerance));
        assert(!cueSetsEqual(a, pastTolerance));
        std::cout << "case 4 (cueSetsEqual position tolerance boundary) OK\n";
    }

    // cueSetsEqual: kind/hotCueNumber mismatches always count, regardless of
    // position. comment is deliberately NOT compared -- RekordboxCueWriter
    // can't write it at all (anlz_cue_codec.cpp always encodes an empty
    // comment), so treating a comment difference as a real mismatch made
    // Engine cues with a label permanently reappear as "needs sync" with no
    // writer able to ever resolve it. color IS compared for hot cues (both
    // formats support it), but NOT for memory cues -- Engine's single
    // memory-style cue point has no color at all, so the same
    // permanently-stuck problem would hit any colored rekordbox memory cue.
    {
        CuePoint base{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"};
        assert(!cueSetsEqual({base}, {CuePoint{CuePoint::Kind::Memory, 1, 1000.0, "#FF0000", "drop"}}));
        assert(!cueSetsEqual({base}, {CuePoint{CuePoint::Kind::Hot, 2, 1000.0, "#FF0000", "drop"}}));
        assert(!cueSetsEqual({base}, {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#00FF00", "drop"}}));
        assert(cueSetsEqual({base}, {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "break"}}));

        CuePoint memoryBase{CuePoint::Kind::Memory, 0, 7000.0, "#FF0000", ""};
        assert(cueSetsEqual({memoryBase}, {CuePoint{CuePoint::Kind::Memory, 0, 7000.0, "", ""}}));
        std::cout << "case 5 (cueSetsEqual field mismatches -> false, comment/memory-color ignored) OK\n";
    }

    // titleArtistKey: case/whitespace-insensitive, symmetric in what it
    // normalizes, and nullopt when either field is missing (too weak a
    // signal to match on).
    {
        Track a;
        a.title = "Song Title";
        a.artist = "The Artist";
        Track b;
        b.title = "song title";
        b.artist = "the artist";
        assert(titleArtistKey(a) == titleArtistKey(b));

        Track noTitle;
        noTitle.artist = "The Artist";
        assert(!titleArtistKey(noTitle).has_value());

        Track noArtist;
        noArtist.title = "Song Title";
        assert(!titleArtistKey(noArtist).has_value());

        Track differentArtist;
        differentArtist.title = "Song Title";
        differentArtist.artist = "Someone Else";
        assert(titleArtistKey(a) != titleArtistKey(differentArtist));
        std::cout << "case 6 (titleArtistKey normalization and missing-field handling) OK\n";
    }

    // matchTracks: title+artist is primary (filenames may legitimately
    // differ), filename is only a fallback when metadata is missing.
    {
        Track a1;
        a1.sourceId = "a1";
        a1.filename = "01 - song.mp3";
        a1.title = "Song";
        a1.artist = "Artist";
        a1.durationSeconds = 200.0;

        Track b1;
        b1.sourceId = "b1";
        b1.filename = "song (export).mp3";
        b1.title = "Song";
        b1.artist = "Artist";
        b1.durationSeconds = 200.5;

        std::vector<Track> as = {a1};
        std::vector<Track> bs = {b1};
        auto matches = matchTracks(as, bs);
        assert(matches.size() == 1);
        assert(matches[0].first->sourceId == "a1");
        assert(matches[0].second->sourceId == "b1");
        std::cout << "case 7 (matchTracks: title+artist match despite differing filenames) OK\n";
    }
    {
        Track a2;
        a2.sourceId = "a2";
        a2.filename = "same.mp3";
        a2.durationSeconds = 100.0;

        Track b2;
        b2.sourceId = "b2";
        b2.filename = "same.mp3";
        b2.durationSeconds = 100.0;

        std::vector<Track> as = {a2};
        std::vector<Track> bs = {b2};
        auto matches = matchTracks(as, bs);
        assert(matches.size() == 1);
        assert(matches[0].first->sourceId == "a2");
        assert(matches[0].second->sourceId == "b2");
        std::cout << "case 8 (matchTracks: filename fallback when metadata missing) OK\n";
    }

    // matchTracks: exact file path is decisive on its own, even when
    // title/artist/duration all differ wildly -- the two rows describe
    // the same physical file on the same stick, which is stronger
    // evidence than any metadata heuristic. Confirmed on real data: 100%
    // of rekordbox tracks matched their Engine counterpart by path,
    // vs. ~94% by title+artist+duration even after fixing that
    // heuristic's own duration-0 bug (see case 9 below).
    {
        Track a;
        a.sourceId = "a";
        a.filePath = "/stick/Contents/Artist/Song.mp3";
        a.title = "Totally Different Title";
        a.artist = "Totally Different Artist";
        a.durationSeconds = 200.0;

        Track b;
        b.sourceId = "b";
        b.filePath = "/stick/Contents/Artist/Song.mp3";
        b.title = "Not Even Close";
        b.artist = "Someone Else";
        b.durationSeconds = 45.0;

        std::vector<Track> as = {a};
        std::vector<Track> bs = {b};
        auto matches = matchTracks(as, bs);
        assert(matches.size() == 1);
        assert(matches[0].first->sourceId == "a");
        assert(matches[0].second->sourceId == "b");
        std::cout << "case 8b (matchTracks: exact file path match wins regardless of metadata) OK\n";
    }

    // matchTracks: a duration of 0 means "unreadable" (same fallback
    // convention as the rest of Track's fields), not a real zero-length
    // track -- a track whose duration failed to read on one side must
    // still match its counterpart by title+artist. Regression case:
    // found on real data, this exact bug silently failed to match 1207
    // of 1566 Engine tracks against their rekordbox counterpart during
    // Sync Cue Points, so most cues never actually propagated across
    // formats.
    {
        Track engineTrack;
        engineTrack.sourceId = "engine1";
        engineTrack.title = "In My Head";
        engineTrack.artist = "Domek";
        engineTrack.durationSeconds = 0.0;  // unreadable, not "really 0 seconds"

        Track rekordboxTrack;
        rekordboxTrack.sourceId = "rb1";
        rekordboxTrack.title = "In My Head";
        rekordboxTrack.artist = "Domek";
        rekordboxTrack.durationSeconds = 462.0;

        std::vector<Track> engineTracks = {engineTrack};
        std::vector<Track> rekordboxTracks = {rekordboxTrack};
        auto matches = matchTracks(engineTracks, rekordboxTracks);
        assert(matches.size() == 1);
        assert(matches[0].first->sourceId == "engine1");
        assert(matches[0].second->sourceId == "rb1");
        std::cout << "case 9 (matchTracks: unreadable duration on one side doesn't block a real match) OK\n";
    }

    // matchTracks: a genuine duration mismatch (both sides have a real
    // reading) still correctly rejects the match -- the fix above must
    // not turn into "always match on title+artist regardless of
    // duration."
    {
        Track a;
        a.sourceId = "a";
        a.title = "Song";
        a.artist = "Artist";
        a.durationSeconds = 200.0;

        Track b;
        b.sourceId = "b";
        b.title = "Song";
        b.artist = "Artist";
        b.durationSeconds = 45.0;  // a genuinely different-length track, e.g. an intro edit

        std::vector<Track> as = {a};
        std::vector<Track> bs = {b};
        auto matches = matchTracks(as, bs);
        assert(matches.empty());
        std::cout << "case 10 (matchTracks: a real duration mismatch still rejects the match) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
