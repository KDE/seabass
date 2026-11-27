// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "domain/playlist_sync.hpp"
#include "domain/tracks_in_no_playlist.hpp"

namespace seabass::domain
{

// What deleting a playlist together with its tracks comes to (Browse,
// "Delete playlist and its tracks"), read from every library on the stick
// at once: the question is about the stick's files, never about one
// library.
//
// A track is the playlist's when any library lists its file there (or in
// a playlist inside it, when the path is a folder's): the libraries need
// not agree on what the playlist holds. It goes only when no library
// lists the file in any other playlist; one that another playlist uses,
// in whichever library, stays on the stick.
struct PlaylistTracksDeletion
{
    // The libraries listing a track in the playlist, in the catalogs'
    // order. One holding the playlist empty is not here: its memberships
    // cannot say so.
    std::vector<std::string> librariesListing;
    // In no other playlist anywhere: these go, with every library's row
    // for the file (one per row). By title, then path.
    std::vector<TrackInNoPlaylist> going;
    // In another playlist in some library: these stay. Same order.
    std::vector<TrackInNoPlaylist> staying;
    // Tracks of the playlist with no file (streaming), as many as the
    // library with the most of them has: nothing to delete.
    int withoutFile = 0;
};

// `key` spells a file path the way the caller compares paths (the
// application's normalizedPathKey). `playlist` is its full path on the
// stick ("Folder/List").
//
// `leftAlone` names the libraries (catalog formats) the delete does not
// reach because the node at that path is a folder (rekordbox, OneLibrary)
// or an Engine playlist holding others: it stays there with everything in
// it. What such a library lists there, or inside it, is then another
// playlist's: no track is the playlist's on that library's word, and one
// it lists stays on the stick.
PlaylistTracksDeletion planPlaylistTracksDeletion(const std::vector<PlaylistCatalog> &catalogs,
                                                  const std::function<std::string(const std::string &)> &key,
                                                  const std::string &playlist,
                                                  const std::vector<std::string> &leftAlone = {});

}  // namespace seabass::domain
