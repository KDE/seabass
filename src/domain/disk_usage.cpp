// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/disk_usage.hpp"

#include <algorithm>
#include <map>

namespace seabass::domain
{

namespace
{

template <typename KeyFn>
DiskUsageNode groupBySize(const std::vector<Track> &tracks, std::size_t topN, const std::string &emptyKeyLabel,
                           const std::string &otherLabel, KeyFn &&keysFor)
{
    std::map<std::string, std::uint64_t> sizeByKey;
    for (const auto &track : tracks) {
        if (track.fileSizeBytes == 0) {
            continue;
        }
        auto keys = keysFor(track);
        if (keys.empty()) {
            sizeByKey[emptyKeyLabel] += track.fileSizeBytes;
        } else {
            for (const auto &key : keys) {
                sizeByKey[key] += track.fileSizeBytes;
            }
        }
    }

    std::vector<std::pair<std::string, std::uint64_t>> sorted(sizeByKey.begin(), sizeByKey.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });

    DiskUsageNode root;
    std::uint64_t otherTotal = 0;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        if (i < topN) {
            root.children.push_back({sorted[i].first, sorted[i].second, {}});
            root.sizeBytes += sorted[i].second;
        } else {
            otherTotal += sorted[i].second;
        }
    }
    if (otherTotal > 0) {
        root.children.push_back({otherLabel, otherTotal, {}});
        root.sizeBytes += otherTotal;
    }
    return root;
}

}  // namespace

DiskUsageNode DiskUsageAnalyzer::byArtist(const std::vector<Track> &tracks, std::size_t topN)
{
    DiskUsageNode node = groupBySize(tracks, topN, "Unknown artist", "Other artists",
                                      [](const Track &t) -> std::vector<std::string> {
                                          if (t.artist.empty()) {
                                              return {};
                                          }
                                          return {t.artist};
                                      });
    node.label = "By artist";
    return node;
}

DiskUsageNode DiskUsageAnalyzer::byPlaylist(const std::vector<Track> &tracks, std::size_t topN)
{
    DiskUsageNode node = groupBySize(tracks, topN, "Unfiled tracks", "Other playlists",
                                      [](const Track &t) -> std::vector<std::string> {
                                          std::vector<std::string> names;
                                          names.reserve(t.playlists.size());
                                          for (const auto &pl : t.playlists) {
                                              names.push_back(pl.name);
                                          }
                                          return names;
                                      });
    node.label = "By playlist";
    return node;
}

}  // namespace seabass::domain
