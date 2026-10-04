// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <map>
#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// One 10-BPM-wide bucket, e.g. rangeStart == 120 covers [120, 130).
struct BpmBucket
{
    int rangeStart = 0;
    int count = 0;
};

// One bar of the cues-per-track histogram: the tracks carrying between
// minCues and maxCues cue points, both inclusive. maxCues == -1 is open
// ended ("9 or more").
struct CueCountBucket
{
    int minCues = 0;
    int maxCues = 0;
    int count = 0;
};

// "3", "4-5" or "9+": the bucket's name under its bar.
std::string cueCountBucketLabel(const CueCountBucket &bucket);

// How many of a catalog's local tracks carry at least one cue point, and
// how the cue counts spread. A cue is anything in Track::cues: a hot cue,
// a memory cue or a loop, whichever list the reader found it in.
// Streaming tracks are left out: they have no file on the stick (the
// disk-usage figure skips them too), so "prepared or not" is not a
// question this stick can answer for them. withCues + withoutCues is the
// local track count, and so is the sum of the histogram's buckets.
struct CueCoverage
{
    int withCues = 0;
    int withoutCues = 0;
    // Always the same seven buckets in this order: 0, 1, 2, 3, 4-5, 6-8,
    // 9+, empty ones included, so a chart of it keeps its shape.
    std::vector<CueCountBucket> cuesPerTrack;
};

// Pure: counts over the given tracks only.
CueCoverage calculateCueCoverage(const std::vector<Track> &tracks);

// Aggregate statistics for one catalog's track list (rekordbox, Engine, or
// OneLibrary -- callers pass one format's tracks at a time, same
// "never mix catalogs" convention as LibraryConsistencyChecker).
struct LibraryStatistics
{
    int trackCount = 0;
    // Every distinct playlist name seen across all tracks' own
    // PlaylistMembership list, not a separate reader-level enumeration --
    // an empty playlist (no tracks in it) is invisible to this count,
    // since nothing in domain::Track can see one. Good enough for "how
    // many playlists is this DJ actually using."
    int playlistCount = 0;
    int totalCuePoints = 0;
    int hotCueCount = 0;
    int memoryCueCount = 0;
    int ratedTrackCount = 0;
    int commentedTrackCount = 0;  // non-empty Track::comment
    int streamingTrackCount = 0;

    std::map<std::string, int> tracksPerKey;              // musical key -> count; "" bucket is "unknown/no key"
    std::map<std::string, int> tracksPerFileFormat;       // lowercase extension (no dot) -> count
    std::map<std::string, int> streamingTracksByService;  // e.g. "TIDAL" -> count
    std::vector<BpmBucket> bpmDistribution;               // sorted by rangeStart, tracks with bpm <= 0 excluded
    CueCoverage cueCoverage;                              // local tracks only, see CueCoverage
};

class LibraryStatisticsCalculator
{
public:
    static LibraryStatistics calculate(const std::vector<Track> &tracks);
};

}  // namespace seabass::domain
