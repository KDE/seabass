// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "domain/playlist_sync.hpp"
#include "domain/track.hpp"

namespace seabass::domain
{

// A file on the stick that no library lists in any playlist (Library
// Health, "Tracks not in any playlist"). History lists do not count as
// playlists: no reader returns them as memberships.
struct TrackInNoPlaylist
{
    std::string filePath;
    std::string title;
    std::string artist;
    // Every library's row for the file, one per row (a file imported twice
    // into one library has two).
    std::vector<Track> rows;
};

struct NoPlaylistScan
{
    std::vector<TrackInNoPlaylist> tracks;  // by title, then path
    // Whether any library on the stick has a playlist with a track in it.
    // A stick without one lists every track here, which says nothing
    // about which tracks are unwanted, so the page offers no deletion.
    bool stickHasPlaylists = false;
};

// `key` spells a file path the way the caller compares paths (the
// application's normalizedPathKey). Rows with no file (streaming) are
// left out.
NoPlaylistScan findTracksInNoPlaylist(const std::vector<PlaylistCatalog> &catalogs,
                                      const std::function<std::string(const std::string &)> &key);

}  // namespace seabass::domain
