// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include "domain/track_scope.hpp"

using namespace seabass::domain;

namespace
{

Track makeTrack(const std::string &format, const std::string &sourceId, const std::string &title,
                 const std::string &artist, std::vector<std::string> playlistNames = {})
{
    Track track;
    track.format = format;
    track.sourceId = sourceId;
    track.title = title;
    track.artist = artist;
    for (const auto &name : playlistNames) {
        track.playlists.push_back(PlaylistMembership{name, 0});
    }
    return track;
}

}  // namespace

int main()
{
    // all(): every track matches, regardless of content.
    {
        std::vector<Track> tracks = {
            makeTrack("rekordbox", "1", "Song A", "Artist A"),
            makeTrack("engine", "2", "Song B", "Artist B"),
        };
        auto result = filterByScope(tracks, TrackScope::all());
        assert(result.size() == 2);
        std::cout << "case 1 (all() matches everything) OK\n";
    }

    // playlist(): only tracks that are members of the named playlist.
    {
        std::vector<Track> tracks = {
            makeTrack("rekordbox", "1", "In Techno", "Artist A", {"Techno"}),
            makeTrack("rekordbox", "2", "Not In Techno", "Artist B", {"House"}),
            makeTrack("rekordbox", "3", "Also Techno", "Artist C", {"House", "Techno"}),
        };
        auto result = filterByScope(tracks, TrackScope::playlist("Techno"));
        assert(result.size() == 2);
        assert(result[0].sourceId == "1");
        assert(result[1].sourceId == "3");
        std::cout << "case 2 (playlist() filters by membership) OK\n";
    }

    // playlist(): a playlist nothing belongs to yields an empty result.
    {
        std::vector<Track> tracks = {makeTrack("rekordbox", "1", "Song A", "Artist A", {"House"})};
        auto result = filterByScope(tracks, TrackScope::playlist("Techno"));
        assert(result.empty());
        std::cout << "case 3 (playlist() no match -> empty) OK\n";
    }

    // search(): case-insensitive substring against title or artist.
    {
        std::vector<Track> tracks = {
            makeTrack("rekordbox", "1", "Voodoo People", "The Prodigy"),
            makeTrack("rekordbox", "2", "Firestarter", "The Prodigy"),
            makeTrack("rekordbox", "3", "Windowlicker", "Aphex Twin"),
        };
        auto byTitle = filterByScope(tracks, TrackScope::search("voodoo"));
        assert(byTitle.size() == 1 && byTitle[0].sourceId == "1");

        auto byArtist = filterByScope(tracks, TrackScope::search("PRODIGY"));
        assert(byArtist.size() == 2);
        std::cout << "case 4 (search() matches title/artist, case-insensitively) OK\n";
    }

    // arbitrary(): exactly the given (format, sourceId) set, nothing else.
    {
        std::vector<Track> tracks = {
            makeTrack("rekordbox", "1", "Song A", "Artist A"),
            makeTrack("rekordbox", "2", "Song B", "Artist B"),
            makeTrack("engine", "1", "Song A (Engine copy)", "Artist A"),
            makeTrack("rekordbox", "3", "Song C", "Artist C"),
        };
        // Picked deterministically (every other track), not truly at
        // random -- a random pick would make a failure impossible to
        // reproduce.
        std::set<TrackId> picked = {{"rekordbox", "1"}, {"rekordbox", "3"}};
        auto result = filterByScope(tracks, TrackScope::arbitrary(picked));
        assert(result.size() == 2);
        assert(result[0].sourceId == "1" && result[0].format == "rekordbox");
        assert(result[1].sourceId == "3" && result[1].format == "rekordbox");
        std::cout << "case 5 (arbitrary() matches exactly the given ids) OK\n";
    }

    // arbitrary(): format matters, not just sourceId -- an id from a
    // different format never matches, even with the same sourceId string.
    {
        std::vector<Track> tracks = {makeTrack("engine", "1", "Song A (Engine copy)", "Artist A")};
        std::set<TrackId> picked = {{"rekordbox", "1"}};
        auto result = filterByScope(tracks, TrackScope::arbitrary(picked));
        assert(result.empty());
        std::cout << "case 6 (arbitrary() distinguishes format, not just sourceId) OK\n";
    }

    // isAll() reflects only the all() scope.
    {
        assert(TrackScope::all().isAll());
        assert(!TrackScope::playlist("Techno").isAll());
        assert(!TrackScope::search("x").isAll());
        assert(!TrackScope::arbitrary({}).isAll());
        std::cout << "case 7 (isAll() true only for all()) OK\n";
    }

    std::cout << "All track_scope tests passed.\n";
    return 0;
}
