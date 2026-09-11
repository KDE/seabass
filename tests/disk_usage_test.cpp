// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include "domain/disk_usage.hpp"

using namespace seabass::domain;

namespace
{

Track makeTrack(std::string artist, std::uint64_t sizeBytes, std::vector<PlaylistMembership> playlists = {})
{
    Track t;
    t.sourceId = artist + std::to_string(sizeBytes);
    t.artist = std::move(artist);
    t.fileSizeBytes = sizeBytes;
    t.playlists = std::move(playlists);
    return t;
}

}  // namespace

int main()
{
    // Case 1: byArtist groups and sums by artist, largest first.
    {
        std::vector<Track> tracks = {
            makeTrack("Domek", 100),
            makeTrack("Domek", 50),
            makeTrack("Underworld", 200),
        };
        auto node = DiskUsageAnalyzer::byArtist(tracks);
        assert(node.sizeBytes == 350);
        assert(node.children.size() == 2);
        assert(node.children[0].label == "Underworld");
        assert(node.children[0].sizeBytes == 200);
        assert(node.children[1].label == "Domek");
        assert(node.children[1].sizeBytes == 150);
        std::cout << "case 1 (byArtist groups and sorts) OK\n";
    }

    // Case 2: an empty artist falls into the "Unknown artist" bucket, a
    // zero-size track (unresolved file) contributes nothing.
    {
        Track noArtist = makeTrack("", 100);
        Track zeroSize = makeTrack("Someone", 0);
        auto node = DiskUsageAnalyzer::byArtist({noArtist, zeroSize});
        assert(node.sizeBytes == 100);
        assert(node.children.size() == 1);
        assert(node.children[0].label == "Unknown artist");
        std::cout << "case 2 (unknown artist bucket, zero-size skipped) OK\n";
    }

    // Case 3: topN folds the rest into "Other artists".
    {
        std::vector<Track> tracks = {
            makeTrack("A", 500),
            makeTrack("B", 400),
            makeTrack("C", 300),
            makeTrack("D", 200),
            makeTrack("E", 100),
        };
        auto node = DiskUsageAnalyzer::byArtist(tracks, /*topN=*/2);
        assert(node.children.size() == 3);
        assert(node.children[0].label == "A");
        assert(node.children[1].label == "B");
        assert(node.children[2].label == "Other artists");
        assert(node.children[2].sizeBytes == 300 + 200 + 100);
        std::cout << "case 3 (topN folds remainder into Other) OK\n";
    }

    // Case 4: byPlaylist attributes a track's full size to every playlist
    // it's in (overlapping, not a strict partition), and an unfiled track
    // goes to its own bucket.
    {
        Track inTwo = makeTrack("X", 100, {{"Techno", 0}, {"Peak Time", 0}});
        Track unfiled = makeTrack("Y", 50, {});
        auto node = DiskUsageAnalyzer::byPlaylist({inTwo, unfiled});
        assert(node.children.size() == 3);
        std::uint64_t technoSize = 0, peakSize = 0, unfiledSize = 0;
        for (const auto &child : node.children) {
            if (child.label == "Techno") technoSize = child.sizeBytes;
            if (child.label == "Peak Time") peakSize = child.sizeBytes;
            if (child.label == "Unfiled tracks") unfiledSize = child.sizeBytes;
        }
        assert(technoSize == 100);
        assert(peakSize == 100);
        assert(unfiledSize == 50);
        std::cout << "case 4 (byPlaylist overlapping attribution) OK\n";
    }

    std::cout << "All disk_usage tests passed.\n";
    return 0;
}
