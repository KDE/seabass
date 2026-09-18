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
    // position. Neither comment nor color is compared. comment because
    // RekordboxCueWriter cannot write one at all (anlz_cue_codec.cpp
    // always encodes an empty comment), so a comment difference made
    // Engine cues with a label reappear as "needs sync" forever, with no
    // writer able to settle it. color because it is extra information
    // about a cue rather than what the cue is: a cue in the same slot at
    // the same position in another shade is the same cue, and calling
    // that a disagreement put a permanent conflict on a pair nobody
    // needed to resolve. Colour is carried instead -- see
    // keepExistingColours below.
    {
        CuePoint base{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"};
        assert(!cueSetsEqual({base}, {CuePoint{CuePoint::Kind::Memory, 1, 1000.0, "#FF0000", "drop"}}));
        assert(!cueSetsEqual({base}, {CuePoint{CuePoint::Kind::Hot, 2, 1000.0, "#FF0000", "drop"}}));
        assert(cueSetsEqual({base}, {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#00FF00", "drop"}}));
        // Except no colour at all, which is not a shade but a side that
        // has not been told one: unequal, so the sync carries it across.
        assert(!cueSetsEqual({base}, {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "", "drop"}}));
        assert(cueSetsEqual({base}, {CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "break"}}));

        CuePoint memoryBase{CuePoint::Kind::Memory, 0, 7000.0, "#FF0000", ""};
        assert(cueSetsEqual({memoryBase}, {CuePoint{CuePoint::Kind::Memory, 0, 7000.0, "", ""}}));
        std::cout << "case 5 (cueSetsEqual: slot, kind and position decide; comment and colour do not) OK\n";
    }

    // Colour is not compared, so it has to be carried: a cue arriving
    // with none takes the colour the target already had for it, and one
    // arriving with a colour keeps its own. Silence is not an
    // instruction to erase.
    {
        const std::vector<CuePoint> existing = {
            CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", ""},
            CuePoint{CuePoint::Kind::Hot, 2, 5000.0, "#00FF00", ""},
            CuePoint{CuePoint::Kind::Memory, 0, 7000.0, "#0000FF", ""},
        };
        const std::vector<CuePoint> incoming = {
            CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "", ""},          // silent: inherits red
            CuePoint{CuePoint::Kind::Hot, 2, 5000.0, "#FFFF00", ""},   // brings its own
            CuePoint{CuePoint::Kind::Memory, 0, 7400.0, "", ""},       // same cue, 400 ms off
            CuePoint{CuePoint::Kind::Hot, 3, 9000.0, "", ""},          // nothing to inherit
        };
        const auto kept = keepExistingColours(incoming, existing);
        assert(kept.size() == 4);
        assert(kept[0].color == "#FF0000" && "an empty colour takes what was there");
        assert(kept[1].color == "#FFFF00" && "a real colour is never overwritten");
        assert(kept[2].color == "#0000FF" && "memory cues match by position, within the same tolerance");
        assert(kept[3].color.empty() && "and a cue the target never had stays as it is");
        std::cout << "case 5b (colour is carried across rather than fought over) OK\n";
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
    // track. It cannot be compared, so it stands aside only when there is
    // nothing to tell apart: here the key names exactly one track on each
    // side, so the match holds. Found on real data as the dominant cause of
    // sync match failures (1207 of 1566 Engine tracks), and the committed
    // fixture -- catalogs without audio to fill lengths from -- still
    // depends on it (1161 matches, 188 if unknown lengths never matched).
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
        std::cout << "case 9 (matchTracks: an unreadable duration under an unambiguous title+artist still matches) OK\n";
    }

    // ...but the same file is still the same file. Two rows naming one path
    // on one stick match with no comparison at all, so an unreadable
    // length on either side does not stand in the way.
    {
        Track engineTrack;
        engineTrack.sourceId = "engine1";
        engineTrack.title = "In My Head";
        engineTrack.artist = "Domek";
        engineTrack.filePath = "/media/RV2/Contents/Domek/In My Head.mp3";
        engineTrack.durationSeconds = 0.0;

        Track rekordboxTrack;
        rekordboxTrack.sourceId = "rb1";
        rekordboxTrack.title = "In My Head";
        rekordboxTrack.artist = "Domek";
        rekordboxTrack.filePath = "/media/RV2/Contents/Domek/In My Head.mp3";
        rekordboxTrack.durationSeconds = 462.0;

        std::vector<Track> engineTracks = {engineTrack};
        std::vector<Track> rekordboxTracks = {rekordboxTrack};
        auto matches = matchTracks(engineTracks, rekordboxTracks);
        assert(matches.size() == 1);
        assert(matches[0].first->sourceId == "engine1");
        assert(matches[0].second->sourceId == "rb1");
        std::cout << "case 9b (matchTracks: the same file path matches whatever either length reads) OK\n";
    }

    // ...and an unknown length does not match under a title+artist that
    // names two tracks. The radio edit and the extended mix share artist
    // and title; with one side's length unreadable there is nothing to say
    // which is which, and matching the first would hand it the other's
    // cues. Checked from both sides: the pair can be on either.
    {
        Track unknown;
        unknown.sourceId = "engine1";
        unknown.title = "In My Head";
        unknown.artist = "Domek";
        unknown.durationSeconds = 0.0;

        Track radioEdit;
        radioEdit.sourceId = "rb-radio";
        radioEdit.title = "In My Head";
        radioEdit.artist = "Domek";
        radioEdit.durationSeconds = 212.0;
        Track extendedMix = radioEdit;
        extendedMix.sourceId = "rb-extended";
        extendedMix.durationSeconds = 462.0;

        std::vector<Track> one = {unknown};
        std::vector<Track> two = {radioEdit, extendedMix};
        assert(matchTracks(one, two).empty());
        assert(matchTracks(two, one).empty());
        std::cout << "case 9c (matchTracks: an unreadable duration under a title+artist naming two tracks matches none) OK\n";
    }

    // Two real lengths still have to agree, however unambiguous the key.
    {
        Track engineTrack;
        engineTrack.sourceId = "engine1";
        engineTrack.title = "In My Head";
        engineTrack.artist = "Domek";
        engineTrack.durationSeconds = 212.0;
        Track rekordboxTrack = engineTrack;
        rekordboxTrack.sourceId = "rb1";
        rekordboxTrack.durationSeconds = 462.0;

        std::vector<Track> engineTracks = {engineTrack};
        std::vector<Track> rekordboxTracks = {rekordboxTrack};
        assert(matchTracks(engineTracks, rekordboxTracks).empty());
        std::cout << "case 9d (matchTracks: a unique key does not excuse two real lengths that disagree) OK\n";
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
