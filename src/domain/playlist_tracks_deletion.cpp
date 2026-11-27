// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/playlist_tracks_deletion.hpp"

#include <algorithm>
#include <map>

namespace seabass::domain
{

namespace
{

// The playlist itself, or one inside it when it is a folder.
bool within(const std::string &name, const std::string &playlist)
{
    return name == playlist
           || (name.size() > playlist.size() && name.compare(0, playlist.size(), playlist) == 0 && name[playlist.size()] == '/');
}

}  // namespace

PlaylistTracksDeletion planPlaylistTracksDeletion(const std::vector<PlaylistCatalog> &catalogs,
                                                  const std::function<std::string(const std::string &)> &key,
                                                  const std::string &playlist)
{
    struct File
    {
        bool inPlaylist = false;
        bool elsewhere = false;
        TrackInNoPlaylist entry;
    };
    PlaylistTracksDeletion plan;
    if (playlist.empty()) {
        return plan;
    }
    std::map<std::string, File> files;
    for (const auto &catalog : catalogs) {
        bool lists = false;
        int withoutFile = 0;
        for (const auto &track : catalog.tracks) {
            bool here = false;
            bool elsewhere = false;
            for (const auto &membership : track.playlists) {
                (within(membership.name, playlist) ? here : elsewhere) = true;
            }
            if (track.filePath.empty()) {
                withoutFile += here ? 1 : 0;
                continue;
            }
            File &file = files[key(track.filePath)];
            file.inPlaylist = file.inPlaylist || here;
            file.elsewhere = file.elsewhere || elsewhere;
            lists = lists || here;
            if (file.entry.rows.empty()) {
                file.entry.filePath = track.filePath;
                file.entry.title = track.title;
                file.entry.artist = track.artist;
            }
            file.entry.rows.push_back(track);
        }
        if (lists) {
            plan.librariesListing.push_back(catalog.format);
        }
        // The libraries list the same tracks: the most one of them has,
        // not their sum.
        plan.withoutFile = std::max(plan.withoutFile, withoutFile);
    }
    for (auto &[k, file] : files) {
        if (file.inPlaylist) {
            (file.elsewhere ? plan.staying : plan.going).push_back(std::move(file.entry));
        }
    }
    const auto byTitle = [](const TrackInNoPlaylist &a, const TrackInNoPlaylist &b) {
        return a.title != b.title ? a.title < b.title : a.filePath < b.filePath;
    };
    std::sort(plan.going.begin(), plan.going.end(), byTitle);
    std::sort(plan.staying.begin(), plan.staying.end(), byTitle);
    return plan;
}

}  // namespace seabass::domain
