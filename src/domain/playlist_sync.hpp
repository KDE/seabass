// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// Playlists that do not hold the same tracks in every library on the
// stick (rekordbox's DeviceLibrary, OneLibrary, Engine).
//
// The three are one library in three formats, so a playlist that holds a
// track in one and not in another plays differently depending on which
// player the stick goes into. Only tracks every compared library lists
// count: a file one library does not list at all is a difference in the
// libraries, not in the playlist, and other checks own it (Duplicates
// left in OneLibrary, broken files). Order is not compared.
struct PlaylistCatalog
{
    std::string format;  // "rekordbox", "onelibrary" or "engine"
    std::vector<Track> tracks;
};

struct PlaylistSide
{
    std::string format;
    bool hasPlaylist = false;
    // Members of this library's playlist whose file it lists.
    int members = 0;
    // Tracks some other library's playlist holds, that this library
    // lists, and that this library's playlist lacks: what this side
    // would gain by matching that other side. This library's own rows.
    std::vector<Track> lacking;
    // This library's own members that another library lists and leaves
    // out of its playlist: what this side would lose by matching it.
    std::vector<Track> extra;
    // Members whose file some other compared library does not list at
    // all: counted, never moved.
    int notInEveryLibrary = 0;
};

struct PlaylistDifference
{
    std::string name;  // full path, "Folder/Playlist"
    std::vector<PlaylistSide> sides;
    // The playlist exists in only some of the libraries on the stick.
    bool missingSomewhere() const;
    const PlaylistSide *side(const std::string &format) const;
};

// The catalogs indexed once, by file key and by playlist, for the finder
// and every alignment of one scan. Building it spells every path, which
// is the expensive part; the catalogs must outlive it.
class PlaylistIndex
{
public:
    PlaylistIndex(const std::vector<PlaylistCatalog> &catalogs,
                  const std::function<std::string(const std::string &)> &key);
    ~PlaylistIndex();
    PlaylistIndex(const PlaylistIndex &) = delete;
    PlaylistIndex &operator=(const PlaylistIndex &) = delete;
    struct Data;
    const Data &data() const { return *m_data; }

private:
    std::unique_ptr<Data> m_data;
};

// `key` spells a file path the way the caller compares paths (the
// application's normalizedPathKey), injected so this stays free of the
// filesystem. Playlists are returned by name, only those that differ.
std::vector<PlaylistDifference> findPlaylistDifferences(const std::vector<PlaylistCatalog> &catalogs,
                                                        const std::function<std::string(const std::string &)> &key);
// The same over an index built once. `between` runs before each playlist
// (a caller's cancellation check throws from it).
std::vector<PlaylistDifference> findPlaylistDifferences(const PlaylistIndex &index,
                                                        const std::function<void()> &between = {});

// What making every other library's copy of `difference` match
// `reference` would do, per target format: tracks to add and tracks to
// remove, each as that target's own rows. A target whose playlist is
// missing, or the reference itself, gets nothing.
struct PlaylistAlignment
{
    std::string format;
    std::vector<Track> add;     // the target's rows to add to its playlist
    std::vector<Track> remove;  // the target's rows to take out of it
    // Of `add`, how many replace a member in `remove` that is another
    // copy of the same song: the target's playlist still points at a copy
    // the reference does not list (Clean Up kept the other one), and
    // adding the kept copy without taking the old one out would put the
    // song in the playlist twice.
    int replacements = 0;
};
//
// A target member the reference does not list at all is normally left
// alone (it is a library difference). The one exception is a copy of the
// same song as a track being added: same title and artist, ignoring case
// and surrounding spaces. Then it is taken out as the other goes in.
std::vector<PlaylistAlignment> alignTo(const PlaylistDifference &difference, const std::string &reference,
                                       const std::vector<PlaylistCatalog> &catalogs,
                                       const std::function<std::string(const std::string &)> &key);
std::vector<PlaylistAlignment> alignTo(const PlaylistDifference &difference, const std::string &reference,
                                       const PlaylistIndex &index);

// The "same song" rule alignTo() swaps copies by.
bool sameSong(const Track &a, const Track &b);

}  // namespace seabass::domain
