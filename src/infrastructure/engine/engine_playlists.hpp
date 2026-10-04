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

}  // namespace seabass::infrastructure::engine
