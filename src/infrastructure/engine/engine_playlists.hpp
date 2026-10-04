// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace seabass::infrastructure::engine
{

// Playlist entries in m.db that name a Track row that does not exist. A
// player skips them, and the Engine reader drops them (no Track row
// carries the membership), so nothing reading through it could see them:
// WHALESHARK2's "Whaleshark Cooldown" had 8 (#61). Read straight from
// SQLite, read-only.
struct DanglingPlaylistEntries
{
    std::string playlist;  // the playlist's own title (not its path)
    int entries = 0;
};
std::vector<DanglingPlaylistEntries> danglingPlaylistEntries(const std::string &engineLibraryPath);

// Deletes those entries from the m.db at `databaseFile`. Engine's own
// trigger (trigger_before_delete_PlaylistEntity) relinks each playlist's
// chain around a deleted entry, so the rest keep their order. Returns how
// many were deleted; throws when the database cannot be written.
int removeDanglingPlaylistEntries(const std::string &databaseFile);

// A playlist's membership in the library at `engineLibraryPath` (a write
// root: the stick's library or a save's scratch copy), the playlist named
// by its full path as the Engine reader spells it ("Folder/List"). Add
// appends at the end unless the track is already a member; remove takes
// the track out. Throw when the playlist or the track does not exist.
// Return whether anything changed.
bool addToEnginePlaylist(const std::string &engineLibraryPath, const std::string &playlistPath, std::int64_t trackId);
bool removeFromEnginePlaylist(const std::string &engineLibraryPath, const std::string &playlistPath,
                              std::int64_t trackId);

// Whether the library at `engineLibraryPath` has a playlist at
// playlistPath ("Folder/List"). False when the library cannot be opened.
bool enginePlaylistExists(const std::string &engineLibraryPath, const std::string &playlistPath);

// The ids of the tracks whose file is `filePath` (absolute), by the path
// Engine stores relative to the library folder.
std::vector<std::int64_t> engineTrackIdsForFile(const std::string &engineLibraryPath, const std::string &filePath);

// The ids of every track some playlist in the library holds.
std::vector<std::int64_t> engineTracksInAnyPlaylist(const std::string &engineLibraryPath);

// Removes the tracks with these ids from the library (rows only; the
// files are the caller's). A track still in a playlist is refused with a
// throw, since removing it would leave the playlist pointing at nothing.
// Returns how many were removed.
int removeEngineTracks(const std::string &engineLibraryPath, const std::vector<std::int64_t> &trackIds);

// Deletes the playlist at playlistPath ("Folder/List") with every playlist
// below it, from the library at `engineLibraryPath` (a write root). Each
// playlist's entries are cleared first: m.db only cascades them with
// foreign keys on, which nothing guarantees. Tracks stay. Returns how many
// playlists went (0 when there is no such playlist); throws when the
// database cannot be written or an entry survives.
int deleteEnginePlaylist(const std::string &engineLibraryPath, const std::string &playlistPath);

}  // namespace seabass::infrastructure::engine
