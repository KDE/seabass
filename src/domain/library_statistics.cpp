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

std::string cueCountBucketLabel(const CueCountBucket &bucket)
{
    if (bucket.maxCues < 0) {
        return std::to_string(bucket.minCues) + "+";
    }
    if (bucket.maxCues == bucket.minCues) {
        return std::to_string(bucket.minCues);
    }
    return std::to_string(bucket.minCues) + "-" + std::to_string(bucket.maxCues);
}

CueCoverage calculateCueCoverage(const std::vector<Track> &tracks)
{
    CueCoverage coverage;
    coverage.cuesPerTrack = {{0, 0, 0}, {1, 1, 0}, {2, 2, 0}, {3, 3, 0}, {4, 5, 0}, {6, 8, 0}, {9, -1, 0}};
    for (const auto &track : tracks) {
        if (!track.streamingSource.empty()) {
            continue;
        }
        const int cues = static_cast<int>(track.cues.size());
        if (cues > 0) {
            coverage.withCues++;
        } else {
            coverage.withoutCues++;
        }
        for (auto &bucket : coverage.cuesPerTrack) {
            if (cues >= bucket.minCues && (bucket.maxCues < 0 || cues <= bucket.maxCues)) {
                bucket.count++;
                break;
            }
        }
    }
    return coverage;
}

LibraryStatistics LibraryStatisticsCalculator::calculate(const std::vector<Track> &tracks)
{
    LibraryStatistics stats;
    stats.trackCount = static_cast<int>(tracks.size());
    stats.cueLists = countCueLists(tracks);

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
    stats.cueCoverage = calculateCueCoverage(tracks);

    stats.bpmDistribution.reserve(bpmBucketCounts.size());
    for (const auto &[start, count] : bpmBucketCounts) {
        stats.bpmDistribution.push_back({start, count});
    }

    return stats;
}

}  // namespace seabass::domain
