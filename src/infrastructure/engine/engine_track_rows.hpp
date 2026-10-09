// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace seabass::infrastructure::engine
{

// Track rows in an Engine 2.x/3.x m.db, written with plain SQL where
// libdjinterop would leave rows behind.

// Removes the tracks with these ids from the m.db at `databaseFile` (a
// write root's database: the stick's own or a save's scratch copy), with
// every row that names them, in one transaction:
//
//   - their PlaylistEntity rows, from every playlist. Engine's own trigger
//     (trigger_before_delete_PlaylistEntity) relinks each playlist's
//     nextEntityId chain around a deleted entry, so the other members keep
//     their order;
//   - their PreparelistEntity rows;
//   - their PerformanceData row (a table from 2.20 on, a view over Track
//     before, which goes with the row);
//   - ChangeLog, where it is a table (2.18 to 2.20; a view after): its
//     trackId is set to NULL, which is what its declared foreign key
//     (ON DELETE SET NULL) asks for. The log entry itself stays;
//   - then the Track row.
//
// libdjinterop's remove_track deletes the Track row alone and relies on
// ON DELETE CASCADE, which SQLite only honours with PRAGMA foreign_keys
// on, which nothing turns on; and PlaylistEntity declares no key to Track
// at all. So its removal leaves the track's performance data and playlist
// entries behind.
//
// Deliberately left: the AlbumArt row the track pointed at (rows are
// shared between tracks, and one with no track left is harmless), and the
// audio file (it belongs to rekordbox, or to whoever put it on the stick;
// Delete Tracks queues it separately).
//
// Unlike removeEngineTracks this removes tracks that are in playlists:
// that is what it is for. A caller with a rule about listed tracks applies
// it before calling.
//
// Returns how many Track rows were removed, which is trackIds.size() on
// success. Returns -1 with *error saying why, and the database as it was
// (the transaction is rolled back), when: the database cannot be opened
// or written; an id has no Track row (an unknown id is an error, not a
// skip, so the caller's count cannot quietly differ from what went); an
// id is named twice; or, after the deletes, some table with a trackId
// column still names a removed track (a schema this does not know). On
// success *error is cleared. `error` is required: a removal that failed
// without saying why would leave the caller with a count and no reason.
[[nodiscard]] int removeEngineTrackRows(const std::string &databaseFile, const std::vector<std::int64_t> &trackIds,
                                        std::string *error);

// How many rows in the m.db at `databaseFile` name track `trackId`: its
// Track row plus every row of every table with a trackId column (views
// are not counted; they hold nothing of their own). Read-only. Returns -1
// with *error saying why when the database cannot be read.
[[nodiscard]] int engineRowsNamingTrack(const std::string &databaseFile, std::int64_t trackId, std::string *error);

}  // namespace seabass::infrastructure::engine
