// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// The playlist tree of a rekordbox XML collection (DJ_PLAYLISTS'
// <PLAYLISTS> section), rebuilt from what the tracks themselves say.
//
// Neither catalog stores a tree: both give each track a list of
// PlaylistMembership, whose name is the full path of the playlist it sits
// in ("Techno/Peak Time") plus its position there. The tree is therefore
// implied by those paths, and this is where it gets made explicit --
// rekordbox's XML needs real nesting, a FOLDER node per path segment with
// a PLAYLIST node at the leaf.
struct XmlPlaylistNode
{
    std::string name;  // this node's own segment, not the full path
    // Indices into the track list handed to buildPlaylistTree. Leaf
    // playlists carry tracks, folders carry children; rekordbox's format
    // has no node that does both, so a path that is used as a folder
    // ("Techno") and as a playlist ("Techno" with tracks in it) comes out
    // as a folder with a "Techno" playlist inside it -- see the .cpp.
    std::vector<std::size_t> trackIndices;
    std::vector<XmlPlaylistNode> children;

    bool isFolder() const { return !children.empty(); }
};

// Builds the tree, with every playlist's tracks in their stored order
// (PlaylistMembership::position, ascending; position -1 sorts last,
// keeping its catalog order among equals so the result is stable).
//
// A track that belongs to no playlist appears in the collection but in no
// node: rekordbox is perfectly happy with that, and it is what "the
// collection is everything, playlists are views onto it" means.
XmlPlaylistNode buildPlaylistTree(const std::vector<Track> &tracks);

}  // namespace seabass::domain
