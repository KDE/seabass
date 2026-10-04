// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// domain::findTracksInNoPlaylist: a file counts only when no library lists
// it in any playlist, every library's rows come with it, streaming rows
// are left out, and a stick with no playlists at all says so.

#include <cassert>
#include <iostream>

#include "domain/tracks_in_no_playlist.hpp"

using namespace seabass::domain;

namespace
{

Track row(const std::string &format, const std::string &id, const std::string &path, const std::string &title,
          std::vector<std::string> playlists = {})
{
    Track t;
    t.format = format;
    t.sourceId = id;
    t.filePath = path;
    t.title = title;
    for (const auto &p : playlists) {
        t.playlists.push_back({p, 0});
    }
    return t;
}

std::string lower(const std::string &s)
{
    std::string out = s;
    for (auto &c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

int main()
{
    // 1. a.mp3 is in a playlist in Engine only: not listed. b.mp3 is in
    //    none anywhere: listed with its rows from all three. c.mp3 is only
    //    in rekordbox, in no playlist: listed. A streaming row is never
    //    listed. Paths are compared through the key (case here).
    {
        std::vector<PlaylistCatalog> catalogs = {
            {"rekordbox", {row("rekordbox", "1", "/S/Contents/a.mp3", "A"), row("rekordbox", "2", "/S/Contents/b.mp3", "B"),
                           row("rekordbox", "3", "/S/Contents/c.mp3", "C")}},
            {"onelibrary", {row("onelibrary", "11", "/S/Contents/A.mp3", "A"), row("onelibrary", "12", "/S/Contents/b.mp3", "B")}},
            {"engine", {row("engine", "21", "/S/Contents/a.mp3", "A", {"Peak"}), row("engine", "22", "/S/Contents/B.mp3", "B"),
                        row("engine", "23", "", "Streamed")}},
        };
        const auto scan = findTracksInNoPlaylist(catalogs, lower);
        assert(scan.stickHasPlaylists);
        assert(scan.tracks.size() == 2);
        assert(scan.tracks[0].title == "B" && scan.tracks[0].rows.size() == 3 && "b.mp3 with all three rows");
        assert(scan.tracks[1].title == "C" && scan.tracks[1].rows.size() == 1);
        std::cout << "case 1 (in no playlist in any library, with every library's rows) OK\n";
    }

    // 2. No playlists anywhere: every track is listed, and the scan says
    //    the stick has none, so nothing should be offered for deletion.
    {
        std::vector<PlaylistCatalog> catalogs = {{"rekordbox", {row("rekordbox", "1", "/S/a.mp3", "A"), row("rekordbox", "2", "/S/b.mp3", "B")}}};
        const auto scan = findTracksInNoPlaylist(catalogs, lower);
        assert(!scan.stickHasPlaylists && scan.tracks.size() == 2);
        std::cout << "case 2 (a stick without playlists says so) OK\n";
    }
    std::cout << "tracks_in_no_playlist_test: all cases passed\n";
    return 0;
}
