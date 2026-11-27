// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// domain::planPlaylistTracksDeletion: a track is the playlist's when any
// library lists it there, it goes only when no library lists it in any
// other playlist, every library's rows come with it, a folder's path takes
// the playlists inside it, a name that merely starts the same is another
// playlist, and a library the delete leaves alone keeps what it lists.

#include <cassert>
#include <iostream>

#include "domain/playlist_tracks_deletion.hpp"

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

std::vector<std::string> titles(const std::vector<TrackInNoPlaylist> &tracks)
{
    std::vector<std::string> out;
    for (const auto &t : tracks) {
        out.push_back(t.title);
    }
    return out;
}

}  // namespace

int main()
{
    // 1. "Peak" across three libraries that do not agree on it.
    //    a: in Peak everywhere, nowhere else: goes, with all three rows.
    //    b: in Peak in rekordbox, and in "Warm" in Engine only: stays.
    //    c: in Peak in Engine only (the others have the row, unlisted):
    //       it is the playlist's, and goes with all three rows.
    //    d: in "Peak Time" only, a name that starts the same: untouched.
    //    e: in no playlist at all: not the playlist's, untouched.
    //    A streaming row in Peak has no file: counted, never deleted.
    //    Paths are compared through the key (case here).
    {
        std::vector<PlaylistCatalog> catalogs = {
            {"rekordbox",
             {row("rekordbox", "1", "/S/Contents/a.mp3", "A", {"Peak"}), row("rekordbox", "2", "/S/Contents/b.mp3", "B", {"Peak"}),
              row("rekordbox", "3", "/S/Contents/c.mp3", "C"), row("rekordbox", "4", "/S/Contents/d.mp3", "D", {"Peak Time"}),
              row("rekordbox", "5", "/S/Contents/e.mp3", "E")}},
            {"onelibrary",
             {row("onelibrary", "11", "/S/Contents/A.mp3", "A", {"Peak"}), row("onelibrary", "12", "/S/Contents/b.mp3", "B", {"Peak"}),
              row("onelibrary", "13", "/S/Contents/c.mp3", "C")}},
            {"engine",
             {row("engine", "21", "/S/Contents/a.mp3", "A", {"Peak"}), row("engine", "22", "/S/Contents/B.mp3", "B", {"Peak", "Warm"}),
              row("engine", "23", "/S/Contents/c.mp3", "C", {"Peak"}), row("engine", "24", "", "Streamed", {"Peak"})}},
            {"onelibrary-again", {row("onelibrary", "31", "", "Streamed", {"Peak"}), row("onelibrary", "32", "", "Other", {"Warm"})}},
        };
        const auto plan = planPlaylistTracksDeletion(catalogs, lower, "Peak");
        assert((plan.librariesListing == std::vector<std::string>{"rekordbox", "onelibrary", "engine"})
               && "a library listing only a streaming track there lists no file of it");
        assert((titles(plan.going) == std::vector<std::string>{"A", "C"}));
        assert((titles(plan.staying) == std::vector<std::string>{"B"}));
        assert(plan.going[0].rows.size() == 3 && plan.going[1].rows.size() == 3 && "every library's row comes with the file");
        assert(plan.going[1].rows[0].format == "rekordbox" && plan.going[1].rows[2].format == "engine");
        assert(plan.staying[0].rows.size() == 3);
        assert(plan.withoutFile == 1 && "one streaming track, in two libraries: counted once");
        std::cout << "case 1 (the playlist's tracks across libraries that disagree, another playlist's use keeps one) OK\n";
    }

    // 2. A folder takes the playlists inside it, however deep; a track of
    //    one of them that a playlist outside the folder also holds stays.
    //    Only Engine lists anything there.
    {
        std::vector<PlaylistCatalog> catalogs = {
            {"rekordbox", {row("rekordbox", "1", "/S/a.mp3", "A"), row("rekordbox", "2", "/S/b.mp3", "B")}},
            {"engine",
             {row("engine", "21", "/S/a.mp3", "A", {"Sets/2025/March"}), row("engine", "22", "/S/b.mp3", "B", {"Sets/Old", "Setsuko"}),
              row("engine", "23", "/S/c.mp3", "C", {"Sets/Old", "Sets/2025/March"})}},
        };
        const auto plan = planPlaylistTracksDeletion(catalogs, lower, "Sets");
        assert((plan.librariesListing == std::vector<std::string>{"engine"}));
        assert((titles(plan.going) == std::vector<std::string>{"A", "C"}) && "two playlists of the folder are still only the folder");
        assert((titles(plan.staying) == std::vector<std::string>{"B"}) && "Setsuko is not in Sets");
        assert(plan.going[0].rows.size() == 2 && "the rekordbox row, unlisted, comes too");
        std::cout << "case 2 (a folder's playlists, and a name that only starts the same) OK\n";
    }

    // 3. Nothing lists a track there, or no playlist is named: nothing.
    {
        std::vector<PlaylistCatalog> catalogs = {{"rekordbox", {row("rekordbox", "1", "/S/a.mp3", "A", {"Peak"})}}};
        const auto none = planPlaylistTracksDeletion(catalogs, lower, "Empty");
        assert(none.librariesListing.empty() && none.going.empty() && none.staying.empty() && none.withoutFile == 0);
        const auto unnamed = planPlaylistTracksDeletion(catalogs, lower, "");
        assert(unnamed.going.empty() && unnamed.staying.empty());
        std::cout << "case 3 (an empty playlist, and no playlist named) OK\n";
    }

    // 4. A library the delete leaves alone (its "Sets" a folder, or an
    //    Engine playlist holding others): it keeps what it lists there and
    //    inside, so no track is the playlist's on its word, and one it lists
    //    stays. a: in Engine's Sets and rekordbox's folder: stays. b: in
    //    Engine's Sets only: goes. c: in rekordbox's folder only: not the
    //    playlist's at all.
    {
        std::vector<PlaylistCatalog> catalogs = {
            {"rekordbox",
             {row("rekordbox", "1", "/S/a.mp3", "A", {"Sets/Inside"}), row("rekordbox", "2", "/S/b.mp3", "B"),
              row("rekordbox", "3", "/S/c.mp3", "C", {"Sets/Inside"})}},
            {"engine", {row("engine", "21", "/S/a.mp3", "A", {"Sets"}), row("engine", "22", "/S/b.mp3", "B", {"Sets"})}},
        };
        const auto plan = planPlaylistTracksDeletion(catalogs, lower, "Sets", {"rekordbox"});
        assert((plan.librariesListing == std::vector<std::string>{"engine"}) && "the folder's library lists none of it");
        assert((titles(plan.going) == std::vector<std::string>{"B"}));
        assert((titles(plan.staying) == std::vector<std::string>{"A"}) && "the folder still lists it");
        assert(plan.going[0].rows.size() == 2 && "every library's row comes with the file");
        const auto engineAlone = planPlaylistTracksDeletion(catalogs, lower, "Sets", {"engine"});
        assert((titles(engineAlone.going) == std::vector<std::string>{"C"}) && (titles(engineAlone.staying) == std::vector<std::string>{"A"}));
        std::cout << "case 4 (a library left alone keeps what it lists there) OK\n";
    }
    return 0;
}
