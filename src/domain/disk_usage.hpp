// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// One node in a Filelight-style treemap: a labeled chunk of space, either
// a leaf (children empty) or a category that breaks down further.
struct DiskUsageNode
{
    std::string label;
    std::uint64_t sizeBytes = 0;
    std::vector<DiskUsageNode> children;
};

// Pure grouping/aggregation over already-resolved track file sizes -- no
// filesystem access here (Track::fileSizeBytes is already populated by
// the reader). Real disk-level numbers (actual used/free space, artwork
// file sizes, database file sizes, and anything not accounted for by
// known tracks) are resolved by infrastructure and combined with these
// nodes by the caller, since domain has no business doing filesystem I/O
// (see LibraryConsistencyChecker's own doc comment for the same
// separation).
class DiskUsageAnalyzer
{
public:
    // Groups tracks by artist, summing fileSizeBytes, largest first.
    // Tracks past topN are folded into one "Other artists" leaf rather
    // than producing a node per artist for a library with hundreds of
    // them. Pass tracks already deduplicated by filePath across catalogs
    // (the same physical file read twice, once per catalog, must not be
    // double-counted) -- the caller (infrastructure/GUI) does that
    // dedup, since only it knows which catalogs were actually combined.
    static DiskUsageNode byArtist(const std::vector<Track> &tracks, std::size_t topN = 15);

    // Same idea, grouped by playlist name. A track in zero playlists
    // contributes to an "Unfiled tracks" leaf; a track in several
    // playlists contributes its full size to each (this is a "where does
    // space go by playlist" view, not a true disjoint partition, so
    // summing every child can exceed the audio-files total when playlists
    // overlap -- that's expected, not a bug).
    static DiskUsageNode byPlaylist(const std::vector<Track> &tracks, std::size_t topN = 15);
};

}  // namespace seabass::domain
