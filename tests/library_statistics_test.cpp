// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include "domain/library_statistics.hpp"

using namespace seabass::domain;

namespace
{

CuePoint makeCue(CuePoint::Kind kind)
{
    CuePoint c;
    c.kind = kind;
    return c;
}

Track makeTrack(std::string id, std::string filename)
{
    Track t;
    t.sourceId = std::move(id);
    t.filename = std::move(filename);
    return t;
}

}  // namespace

int main()
{
    // Case 1: empty input.
    {
        auto stats = LibraryStatisticsCalculator::calculate({});
        assert(stats.trackCount == 0);
        assert(stats.playlistCount == 0);
        std::cout << "case 1 (empty input) OK\n";
    }

    // Case 2: cue counting splits hot vs memory correctly.
    {
        Track t = makeTrack("a", "song.mp3");
        t.cues = {makeCue(CuePoint::Kind::Hot), makeCue(CuePoint::Kind::Hot), makeCue(CuePoint::Kind::Memory)};
        auto stats = LibraryStatisticsCalculator::calculate({t});
        assert(stats.totalCuePoints == 3);
        assert(stats.hotCueCount == 2);
        assert(stats.memoryCueCount == 1);
        std::cout << "case 2 (hot/memory cue split) OK\n";
    }

    // Case 3: rating uses nullopt, not 0, as "unrated" -- a track with a
    // real rating (even a low one) still counts as rated.
    {
        Track rated = makeTrack("a", "a.mp3");
        rated.rating = 1;
        Track unrated = makeTrack("b", "b.mp3");
        auto stats = LibraryStatisticsCalculator::calculate({rated, unrated});
        assert(stats.ratedTrackCount == 1);
        std::cout << "case 3 (rating nullopt vs set) OK\n";
    }

    // Case 4: comment counting, streaming source breakdown by service.
    {
        Track commented = makeTrack("a", "a.mp3");
        commented.comment = "banger";
        Track streaming = makeTrack("b", "b.mp3");
        streaming.streamingSource = "TIDAL";
        auto stats = LibraryStatisticsCalculator::calculate({commented, streaming});
        assert(stats.commentedTrackCount == 1);
        assert(stats.streamingTrackCount == 1);
        assert(stats.streamingTracksByService.at("TIDAL") == 1);
        std::cout << "case 4 (comment + streaming service breakdown) OK\n";
    }

    // Case 5: file-format extraction from filename, case-insensitive, no
    // extension falls into the "" bucket.
    {
        Track mp3 = makeTrack("a", "song.MP3");
        Track flac = makeTrack("b", "song.flac");
        Track noExt = makeTrack("c", "song");
        auto stats = LibraryStatisticsCalculator::calculate({mp3, flac, noExt});
        assert(stats.tracksPerFileFormat.at("mp3") == 1);
        assert(stats.tracksPerFileFormat.at("flac") == 1);
        assert(stats.tracksPerFileFormat.at("") == 1);
        std::cout << "case 5 (file format extraction) OK\n";
    }

    // Case 6: BPM bucketing into 10-wide ranges, tracks with bpm <= 0 excluded.
    {
        Track a = makeTrack("a", "a.mp3");
        a.bpm = 128.4;
        Track b = makeTrack("b", "b.mp3");
        b.bpm = 129.9;
        Track c = makeTrack("c", "c.mp3");
        c.bpm = 140.0;
        Track unknown = makeTrack("d", "d.mp3");
        unknown.bpm = 0.0;
        auto stats = LibraryStatisticsCalculator::calculate({a, b, c, unknown});
        int total = 0;
        for (const auto &bucket : stats.bpmDistribution) {
            total += bucket.count;
        }
        assert(total == 3);
        bool found120 = false;
        bool found140 = false;
        for (const auto &bucket : stats.bpmDistribution) {
            if (bucket.rangeStart == 120) {
                assert(bucket.count == 2);
                found120 = true;
            }
            if (bucket.rangeStart == 140) {
                assert(bucket.count == 1);
                found140 = true;
            }
        }
        assert(found120 && found140);
        std::cout << "case 6 (BPM bucketing) OK\n";
    }

    // Case 7: playlist count is the number of distinct playlist names
    // across all tracks, not the number of memberships.
    {
        Track a = makeTrack("a", "a.mp3");
        a.playlists = {{"Techno", 0}, {"Peak Time", 1}};
        Track b = makeTrack("b", "b.mp3");
        b.playlists = {{"Techno", 1}};
        auto stats = LibraryStatisticsCalculator::calculate({a, b});
        assert(stats.playlistCount == 2);
        std::cout << "case 7 (distinct playlist count) OK\n";
    }

    std::cout << "All library_statistics tests passed.\n";
    return 0;
}
