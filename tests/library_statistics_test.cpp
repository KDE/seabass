// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

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

// Not assert(): a check that vanishes under NDEBUG is green for no
// reason, and these are the counts the page draws.
void check(bool ok, const char *what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        std::exit(1);
    }
}

Track withCues(std::string id, std::vector<CuePoint> cues)
{
    Track t = makeTrack(std::move(id), "x.mp3");
    t.cues = std::move(cues);
    return t;
}

CuePoint makeLoop(CuePoint::Kind kind)
{
    CuePoint c = makeCue(kind);
    c.isLoop = true;
    c.loopEndMs = 4000.0;
    return c;
}

int bucketCount(const CueCoverage &coverage, const std::string &label)
{
    for (const auto &bucket : coverage.cuesPerTrack) {
        if (cueCountBucketLabel(bucket) == label) {
            return bucket.count;
        }
    }
    std::cerr << "FAIL: no bucket labelled " << label << "\n";
    std::exit(1);
}

int bucketSum(const CueCoverage &coverage)
{
    int sum = 0;
    for (const auto &bucket : coverage.cuesPerTrack) {
        sum += bucket.count;
    }
    return sum;
}

void testCueCoverage()
{
    // No tracks: zero everywhere, but still the seven buckets in order.
    {
        const auto c = calculateCueCoverage({});
        check(c.withCues == 0 && c.withoutCues == 0, "empty: zero counts");
        const std::vector<std::string> expected = {"0", "1", "2", "3", "4-5", "6-8", "9+"};
        check(c.cuesPerTrack.size() == expected.size(), "empty: seven buckets");
        for (std::size_t i = 0; i < expected.size(); ++i) {
            check(cueCountBucketLabel(c.cuesPerTrack[i]) == expected[i], "bucket labels in order");
            check(c.cuesPerTrack[i].count == 0, "empty: every bucket zero");
        }
        std::cout << "cue coverage: no tracks OK\n";
    }

    // Every track has cues: nothing without, nothing in bucket 0.
    {
        const auto c = calculateCueCoverage({withCues("a", {makeCue(CuePoint::Kind::Hot)}),
                                             withCues("b", {makeCue(CuePoint::Kind::Hot), makeCue(CuePoint::Kind::Memory)})});
        check(c.withCues == 2 && c.withoutCues == 0, "all with cues");
        check(bucketCount(c, "0") == 0 && bucketCount(c, "1") == 1 && bucketCount(c, "2") == 1, "all with cues: buckets");
        std::cout << "cue coverage: all with cues OK\n";
    }

    // A track holding only a loop, and one holding only memory cues,
    // both count as having cues.
    {
        const auto c = calculateCueCoverage({withCues("loop", {makeLoop(CuePoint::Kind::Hot)}),
                                             withCues("memory", {makeCue(CuePoint::Kind::Memory), makeCue(CuePoint::Kind::Memory),
                                                                 makeCue(CuePoint::Kind::Memory)}),
                                             withCues("memoryloop", {makeLoop(CuePoint::Kind::Memory)}),
                                             withCues("none", {})});
        check(c.withCues == 3, "loop-only and memory-only tracks count as with cues");
        check(c.withoutCues == 1, "the bare track counts as without");
        check(bucketCount(c, "1") == 2 && bucketCount(c, "3") == 1 && bucketCount(c, "0") == 1, "loop/memory buckets");
        std::cout << "cue coverage: loop only, memory only OK\n";
    }

    // Bucket edges, streaming tracks left out, and the counts add up.
    {
        std::vector<Track> tracks;
        const std::vector<int> cueCounts = {0, 0, 1, 2, 3, 4, 5, 6, 8, 9, 30};
        for (std::size_t i = 0; i < cueCounts.size(); ++i) {
            tracks.push_back(withCues("t" + std::to_string(i),
                                      std::vector<CuePoint>(cueCounts[i], makeCue(CuePoint::Kind::Hot))));
        }
        Track streamed = withCues("s", {makeCue(CuePoint::Kind::Hot)});
        streamed.streamingSource = "TIDAL";
        tracks.push_back(streamed);
        Track streamedBare = makeTrack("s2", "");
        streamedBare.streamingSource = "TIDAL";
        tracks.push_back(streamedBare);

        const auto stats = LibraryStatisticsCalculator::calculate(tracks);
        const auto &c = stats.cueCoverage;
        check(c.withCues == 9 && c.withoutCues == 2, "streaming tracks excluded from with/without");
        check(c.withCues + c.withoutCues == stats.trackCount - stats.streamingTrackCount, "with + without is the local total");
        check(bucketSum(c) == stats.trackCount - stats.streamingTrackCount, "histogram adds up to the local total");
        check(bucketCount(c, "0") == 2 && bucketCount(c, "1") == 1 && bucketCount(c, "2") == 1 && bucketCount(c, "3") == 1,
              "small buckets");
        check(bucketCount(c, "4-5") == 2 && bucketCount(c, "6-8") == 2 && bucketCount(c, "9+") == 2, "range bucket edges");
        std::cout << "cue coverage: bucket edges, streaming excluded, totals OK\n";
    }
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

    testCueCoverage();

    std::cout << "All library_statistics tests passed.\n";
    // #60: the analysis files whose two generations of cue list disagree,
    // each file once however many rows name it (a padded and an
    // unpadded spelling are one file), from what the cue pass found.
    {
        using Check = Track::CueListsCheck;
        Track a = makeTrack("a", "a.mp3");
        a.analysisFile = "/PIONEER/USBANLZ/P001/00000001/ANLZ0000.DAT";
        a.cueLists = Check::Disagree;
        Track again = a;
        again.analysisFile += "  ";
        Track b = makeTrack("b", "b.mp3");
        b.analysisFile = "/PIONEER/USBANLZ/P001/00000002/ANLZ0000.DAT";
        b.cueLists = Check::Examined;
        Track engine = makeTrack("c", "c.mp3");  // no analysis file: not counted
        auto stats = LibraryStatisticsCalculator::calculate({a, again, b, engine});
        assert(stats.cueLists.files == 2 && stats.cueLists.examined == 2 && stats.cueLists.disagree == 1);
        assert(stats.cueLists.complete());
        assert(describeCueListCount(stats.cueLists) == "1 cue list disagrees");
        b.cueLists = Check::Unreadable;
        stats = LibraryStatisticsCalculator::calculate({a, b});
        assert(!stats.cueLists.complete() && describeCueListCount(stats.cueLists) == "1 analysis file could not be read");
        std::cout << "case cue lists (files counted once, said only when all were examined) OK\n";
    }

    return 0;
}
