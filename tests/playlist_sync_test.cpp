// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Playlists that differ between the libraries on one stick, and what
// matching them to one library would add and remove.

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "domain/playlist_sync.hpp"

using namespace seabass::domain;

namespace
{

Track row(std::string format, std::string id, std::string file, std::vector<std::string> playlists)
{
    Track t;
    t.format = std::move(format);
    t.sourceId = std::move(id);
    t.filePath = "/stick/Contents/" + file;
    t.title = file;
    for (auto &name : playlists) {
        t.playlists.push_back({name, -1});
    }
    return t;
}

std::string key(const std::string &path)
{
    return path;
}

}  // namespace

int main()
{
    // rekordbox and OneLibrary agree; Engine's "Peak" lacks b.mp3 and
    // holds c.mp3, which the others list but leave out of "Peak". d.mp3
    // is only in Engine: a library difference, not a playlist one.
    const std::vector<PlaylistCatalog> catalogs = {
        {"rekordbox", {row("rekordbox", "1", "a.mp3", {"Peak"}), row("rekordbox", "2", "b.mp3", {"Peak"}),
                       row("rekordbox", "3", "c.mp3", {})}},
        {"onelibrary", {row("onelibrary", "11", "a.mp3", {"Peak"}), row("onelibrary", "12", "b.mp3", {"Peak"}),
                        row("onelibrary", "13", "c.mp3", {})}},
        {"engine", {row("engine", "21", "a.mp3", {"Peak", "Warmup"}), row("engine", "22", "b.mp3", {}),
                    row("engine", "23", "c.mp3", {"Peak"}), row("engine", "24", "d.mp3", {"Peak"})}},
    };
    const auto differences = findPlaylistDifferences(catalogs, key);
    assert(differences.size() == 2);
    const PlaylistDifference &peak = differences[0];
    assert(peak.name == "Peak" && !peak.missingSomewhere());
    const PlaylistSide *engine = peak.side("engine");
    assert(engine && engine->members == 3);
    assert(engine->lacking.size() == 1 && engine->lacking[0].sourceId == "22" && "b.mp3, Engine's own row");
    assert(engine->extra.size() == 1 && engine->extra[0].sourceId == "23" && "c.mp3");
    assert(engine->notInEveryLibrary == 1 && "d.mp3 is only in Engine");
    const PlaylistSide *rekordbox = peak.side("rekordbox");
    assert(rekordbox && rekordbox->lacking.size() == 1 && rekordbox->lacking[0].sourceId == "3");
    assert(rekordbox->extra.size() == 1 && rekordbox->extra[0].sourceId == "2");
    const PlaylistDifference &warmup = differences[1];
    assert(warmup.name == "Warmup" && warmup.missingSomewhere());
    assert(!warmup.side("rekordbox")->hasPlaylist && warmup.side("engine")->hasPlaylist);
    std::cout << "case 1 (differences in tracks every library lists, and a playlist missing) OK\n";

    // Matching rekordbox: Engine gains b and loses c, OneLibrary nothing;
    // d stays (rekordbox does not list it).
    const auto toRekordbox = alignTo(peak, "rekordbox", catalogs, key);
    assert(toRekordbox.size() == 1 && toRekordbox[0].format == "engine");
    assert(toRekordbox[0].add.size() == 1 && toRekordbox[0].add[0].sourceId == "22");
    assert(toRekordbox[0].remove.size() == 1 && toRekordbox[0].remove[0].sourceId == "23");
    // Matching Engine: rekordbox and OneLibrary each gain c and lose b.
    const auto toEngine = alignTo(peak, "engine", catalogs, key);
    assert(toEngine.size() == 2);
    for (const auto &alignment : toEngine) {
        assert(alignment.add.size() == 1 && alignment.add[0].title == "c.mp3");
        assert(alignment.remove.size() == 1 && alignment.remove[0].title == "b.mp3");
    }
    std::cout << "case 2 (aligning to one library adds and removes in the others, rows of their own) OK\n";

    // A copy Clean Up replaced: rekordbox's "Set" holds the kept copy
    // (keep.mp3), Engine's still the removed one (old.mp3), which
    // rekordbox no longer lists. Matching rekordbox swaps them in Engine
    // instead of adding the kept copy beside the old one. A member
    // rekordbox does not list that is another song stays.
    {
        auto titled = [](Track t, std::string title, std::string artist) {
            t.title = std::move(title);
            t.artist = std::move(artist);
            return t;
        };
        const std::vector<PlaylistCatalog> copies = {
            {"rekordbox", {titled(row("rekordbox", "1", "keep.mp3", {"Set"}), "Song", "Artist")}},
            {"engine", {titled(row("engine", "21", "keep.mp3", {}), "Song", "Artist"),
                        titled(row("engine", "22", "old.mp3", {"Set"}), " song ", "ARTIST"),
                        titled(row("engine", "23", "mine.mp3", {"Set"}), "Other", "Artist")}},
        };
        const auto diff = findPlaylistDifferences(copies, key);
        assert(diff.size() == 1);
        const auto toRekordbox = alignTo(diff[0], "rekordbox", copies, key);
        assert(toRekordbox.size() == 1);
        assert(toRekordbox[0].add.size() == 1 && toRekordbox[0].add[0].sourceId == "21");
        assert(toRekordbox[0].remove.size() == 1 && toRekordbox[0].remove[0].sourceId == "22"
               && "the old copy goes out, the other song stays");
        assert(toRekordbox[0].replacements == 1);
        std::cout << "case 2b (a replaced copy is swapped, not doubled) OK\n";
    }

    // Agreeing libraries, and one library alone: nothing.
    const std::vector<PlaylistCatalog> agreeing = {
        {"rekordbox", {row("rekordbox", "1", "a.mp3", {"Peak"})}},
        {"engine", {row("engine", "21", "a.mp3", {"Peak"})}},
    };
    assert(findPlaylistDifferences(agreeing, key).empty());
    assert(findPlaylistDifferences({catalogs[0]}, key).empty());
    std::cout << "case 3 (agreeing libraries, or one library, report nothing) OK\n";
    std::cout << "playlist_sync_test: all cases passed\n";
    return 0;
}
