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
};

class LibraryStatisticsCalculator
{
public:
    static LibraryStatistics calculate(const std::vector<Track> &tracks);
};

}  // namespace seabass::domain
