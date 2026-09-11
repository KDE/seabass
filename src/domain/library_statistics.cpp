// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/library_statistics.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace seabass::domain
{

namespace
{

std::string lowerExtension(const std::string &filename)
{
    auto dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= filename.size()) {
        return "";
    }
    std::string ext = filename.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext;
}

}  // namespace

LibraryStatistics LibraryStatisticsCalculator::calculate(const std::vector<Track> &tracks)
{
    LibraryStatistics stats;
    stats.trackCount = static_cast<int>(tracks.size());

    std::set<std::string> playlistNames;
    std::map<int, int> bpmBucketCounts;

    for (const auto &track : tracks) {
        for (const auto &pl : track.playlists) {
            playlistNames.insert(pl.name);
        }

        for (const auto &cue : track.cues) {
            stats.totalCuePoints++;
            if (cue.kind == CuePoint::Kind::Hot) {
                stats.hotCueCount++;
            } else {
                stats.memoryCueCount++;
            }
        }

        if (track.rating.has_value()) {
            stats.ratedTrackCount++;
        }
        if (!track.comment.empty()) {
            stats.commentedTrackCount++;
        }

        if (!track.streamingSource.empty()) {
            stats.streamingTrackCount++;
            stats.streamingTracksByService[track.streamingSource]++;
        }

        stats.tracksPerKey[track.key]++;
        stats.tracksPerFileFormat[lowerExtension(track.filename)]++;

        if (track.bpm > 0.0) {
            int bucketStart = (static_cast<int>(track.bpm) / 10) * 10;
            bpmBucketCounts[bucketStart]++;
        }
    }

    stats.playlistCount = static_cast<int>(playlistNames.size());

    stats.bpmDistribution.reserve(bpmBucketCounts.size());
    for (const auto &[start, count] : bpmBucketCounts) {
        stats.bpmDistribution.push_back({start, count});
    }

    return stats;
}

}  // namespace seabass::domain
