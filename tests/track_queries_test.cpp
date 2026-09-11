// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include "domain/track_queries.hpp"

using namespace seabass::domain;
using namespace std::chrono;

int main()
{
    // rekordbox-style: sort by play count descending, cued tracks excluded.
    {
        Track cued;
        cued.sourceId = "cued";
        cued.playCount = 999;
        cued.cues.push_back(CuePoint{});

        Track lowPlays;
        lowPlays.sourceId = "low";
        lowPlays.playCount = 3;

        Track highPlays;
        highPlays.sourceId = "high";
        highPlays.playCount = 50;

        Track noSignal;
        noSignal.sourceId = "none";

        std::vector<Track> tracks = {cued, lowPlays, highPlays, noSignal};
        auto result = TrackPrioritizer::tracksNeedingCues(tracks);

        assert(result.size() == 3);  // "cued" excluded
        assert(result[0].sourceId == "high");
        assert(result[1].sourceId == "low");
        assert(result[2].sourceId == "none");  // no signal sorts last
        std::cout << "case 1 (play count priority, cued excluded) OK\n";
    }

    // Engine-style: sort by last played descending.
    {
        auto now = system_clock::now();

        Track playedRecently;
        playedRecently.sourceId = "recent";
        playedRecently.lastPlayedAt = now;

        Track playedLongAgo;
        playedLongAgo.sourceId = "old";
        playedLongAgo.lastPlayedAt = now - hours(24 * 365);

        std::vector<Track> tracks = {playedLongAgo, playedRecently};
        auto result = TrackPrioritizer::tracksNeedingCues(tracks);

        assert(result.size() == 2);
        assert(result[0].sourceId == "recent");
        assert(result[1].sourceId == "old");
        std::cout << "case 2 (last played priority) OK\n";
    }

    // limit truncates.
    {
        Track a;
        a.sourceId = "a";
        a.playCount = 10;
        Track b;
        b.sourceId = "b";
        b.playCount = 5;

        std::vector<Track> tracks = {a, b};
        auto result = TrackPrioritizer::tracksNeedingCues(tracks, 1);
        assert(result.size() == 1);
        assert(result[0].sourceId == "a");
        std::cout << "case 3 (limit) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
