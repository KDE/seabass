// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/collapse_catalog_rows.hpp"

#include <algorithm>
#include <map>

#include "application/path_key.hpp"
#include "domain/local_restore.hpp"

namespace seabass::application
{

namespace
{

void fillGapsFrom(domain::Track &into, const domain::Track &from)
{
    if (into.bitrate <= 0) {
        into.bitrate = from.bitrate;
    }
    if (into.durationSeconds <= 0.0) {
        into.durationSeconds = from.durationSeconds;
    }
    if (into.fileSizeBytes == 0) {
        into.fileSizeBytes = from.fileSizeBytes;
    }
    if (into.bpm <= 0.0) {
        into.bpm = from.bpm;
    }
    if (into.key.empty()) {
        into.key = from.key;
    }
    if (into.artworkPath.empty()) {
        into.artworkPath = from.artworkPath;
    }
    if (into.title.empty()) {
        into.title = from.title;
    }
    if (into.artist.empty()) {
        into.artist = from.artist;
    }
    if (into.album.empty()) {
        into.album = from.album;
    }
    if (!into.rating.has_value()) {
        into.rating = from.rating;
    }
    if (into.comment.empty()) {
        into.comment = from.comment;
    }
    if (!into.playCount.has_value()) {
        into.playCount = from.playCount;
    }
    if (!into.lastPlayedAt.has_value()) {
        into.lastPlayedAt = from.lastPlayedAt;
    }
    into.cues = domain::LocalRestorePlanner::mergeCues(into.cues, from.cues);

    // Playlist membership is the union by name, first position kept.
    // A file is in a playlist if any format says so: the formats are
    // meant to carry the same playlists, so a membership only one of
    // them records is one the others are missing, not one that only
    // half-counts. This is what lets a playlist-scoped cleanup see a
    // file whose membership happens to be recorded in the format it is
    // not currently reading. Where two formats disagree about the
    // position within a playlist, the first wins, like every other field
    // here -- and that disagreement is divergence for synchronization to
    // settle, not something to average.
    for (const auto &membership : from.playlists) {
        bool known = std::any_of(into.playlists.begin(), into.playlists.end(),
                                  [&membership](const domain::PlaylistMembership &existing) {
                                      return existing.name == membership.name;
                                  });
        if (!known) {
            into.playlists.push_back(membership);
        }
    }
}

}  // namespace

std::vector<domain::Track> collapseCatalogRows(const std::vector<domain::Track> &rows)
{
    std::vector<domain::Track> files;
    files.reserve(rows.size());
    std::map<std::string, size_t> byPath;  // path key -> index into files

    for (const auto &row : rows) {
        const std::string key = normalizedPathKey(row.filePath);
        // No path, or a streaming row whose path names a cache on
        // another machine: nothing to establish sameness with, so it
        // stands alone rather than being folded into anything.
        if (key.empty() || !row.streamingSource.empty()) {
            files.push_back(row);
            continue;
        }

        auto [it, inserted] = byPath.emplace(key, files.size());
        if (inserted) {
            files.push_back(row);
            files.back().catalogRows.push_back({row.format, row.sourceId});
            continue;
        }
        domain::Track &existing = files[it->second];
        existing.catalogRows.push_back({row.format, row.sourceId});
        fillGapsFrom(existing, row);
    }

    return files;
}

CleanupScanRows collapseForCleanupScan(const CatalogTracks &catalogs,
                                        const std::vector<std::string> &unreadableCatalogs,
                                        std::vector<domain::Track> singleCatalogRows)
{
    auto withoutStreaming = [](std::vector<domain::Track> tracks) {
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                     [](const domain::Track &t) { return !t.streamingSource.empty(); }),
                     tracks.end());
        return tracks;
    };

    if (!unreadableCatalogs.empty()) {
        return {withoutStreaming(std::move(singleCatalogRows)), false};
    }

    std::vector<domain::Track> rows;
    // Fixed order, because collapseCatalogRows() keeps the first row for
    // a file as the base and callers are entitled to a stable result.
    for (const auto *catalog : {&catalogs.rekordbox, &catalogs.engine, &catalogs.oneLibrary}) {
        if (catalog->has_value()) {
            rows.insert(rows.end(), (*catalog)->begin(), (*catalog)->end());
        }
    }
    rows = withoutStreaming(std::move(rows));
    if (rows.empty()) {
        // No catalog supplied anything. Falling through to an empty
        // collapse would report a cross-catalog scan that saw nothing,
        // which reads as "this stick has no duplicates" rather than "this
        // scan was handed nothing".
        return {withoutStreaming(std::move(singleCatalogRows)), false};
    }
    return {collapseCatalogRows(rows), true};
}

}  // namespace seabass::application