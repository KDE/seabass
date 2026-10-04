// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/tracks_in_no_playlist.hpp"

#include <algorithm>
#include <map>

namespace seabass::domain
{

NoPlaylistScan findTracksInNoPlaylist(const std::vector<PlaylistCatalog> &catalogs,
                                      const std::function<std::string(const std::string &)> &key)
{
    struct File
    {
        bool inPlaylist = false;
        TrackInNoPlaylist entry;
    };
    NoPlaylistScan scan;
    std::map<std::string, File> files;
    for (const auto &catalog : catalogs) {
        for (const auto &track : catalog.tracks) {
            if (track.filePath.empty()) {
                continue;
            }
            File &file = files[key(track.filePath)];
            if (!track.playlists.empty()) {
                file.inPlaylist = true;
                scan.stickHasPlaylists = true;
            }
            if (file.entry.rows.empty()) {
                file.entry.filePath = track.filePath;
                file.entry.title = track.title;
                file.entry.artist = track.artist;
            }
            file.entry.rows.push_back(track);
        }
    }
    for (auto &[k, file] : files) {
        if (!file.inPlaylist) {
            scan.tracks.push_back(std::move(file.entry));
        }
    }
    std::sort(scan.tracks.begin(), scan.tracks.end(), [](const auto &a, const auto &b) {
        return a.title != b.title ? a.title < b.title : a.filePath < b.filePath;
    });
    return scan;
}

}  // namespace seabass::domain
