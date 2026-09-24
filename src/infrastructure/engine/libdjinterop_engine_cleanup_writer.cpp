// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/libdjinterop_engine_cleanup_writer.hpp"
#include "infrastructure/paths/utf8_path.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>

#include <djinterop/djinterop.hpp>
#include <sqlite3.h>

namespace seabass::infrastructure::engine
{

namespace
{

// Where the database sits: 1.x keeps m.db at the library root, 2.x and
// 3.x under Database2. Empty when neither is there.
std::string engineDatabaseFile(const std::string &engineLibraryPath)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = pathFromUtf8(engineLibraryPath);
    fs::path candidate = root / "Database2" / "m.db";
    if (fs::exists(candidate, ec)) {
        return pathToUtf8(candidate);
    }
    candidate = root / "m.db";
    if (fs::exists(candidate, ec)) {
        return pathToUtf8(candidate);
    }
    return {};
}

// If `pl` contains `doomed`, rebuilds its track list with `doomed`
// replaced by `survivor` (or just dropped, if survivor was already
// elsewhere in the same playlist -- never adding a duplicate), then
// recurses into child playlists.
//
// Deliberately does NOT use djinterop::playlist::remove_track(): in
// this vendored libdjinterop version, it deletes by
// `PlaylistEntity.id` (that row's own autoincrement primary key) but
// is passed the *track's* id, which is a different column entirely
// (confirmed against the real schema SQL and libdjinterop's own
// correctly-implemented internal `playlist_entity_table::get(listId,
// trackId)`, which isn't part of the public API). It only "removes"
// anything when a PlaylistEntity row's own id happens to coincide with
// the track id by chance (true for a fresh single-playlist database,
// false in general) -- otherwise it silently deletes zero rows. Caught
// by a real test hanging (a `while (still contains) remove_track()`
// loop that never terminated) rather than assumed away.
// clear_tracks()/add_track_back()/tracks() don't have this problem
// (clear_tracks() only filters by listId; verified against the real
// schema SQL), so rebuilding the list is the safe path.
void reassignPlaylistMembership(djinterop::playlist pl, const djinterop::track &doomed, const djinterop::track &survivor)
{
    auto tracks = pl.tracks();
    bool containsDoomed = false;
    bool containsSurvivor = false;
    for (const auto &t : tracks) {
        if (t.id() == doomed.id()) {
            containsDoomed = true;
        }
        if (t.id() == survivor.id()) {
            containsSurvivor = true;
        }
    }

    if (containsDoomed) {
        pl.clear_tracks();
        bool addedSurvivor = containsSurvivor;
        for (const auto &t : tracks) {
            if (t.id() == doomed.id()) {
                if (!addedSurvivor) {
                    pl.add_track_back(survivor);
                    addedSurvivor = true;
                }
                continue;
            }
            pl.add_track_back(t);
        }
    }

    for (auto &child : pl.children()) {
        reassignPlaylistMembership(child, doomed, survivor);
    }
}

}  // namespace

LibdjinteropEngineCleanupWriter::LibdjinteropEngineCleanupWriter(std::string engineLibraryPath)
    : m_engineLibraryPath(std::move(engineLibraryPath))
{
}

void LibdjinteropEngineCleanupWriter::removeTrackReplacingWith(const std::string &doomedTrackId,
                                                                 const std::string &survivorTrackId)
{
    auto db = djinterop::engine::load_database(m_engineLibraryPath);

    auto doomed = db.track_by_id(std::stoll(doomedTrackId));
    if (!doomed) {
        throw std::runtime_error("no Engine track with id=" + doomedTrackId);
    }
    auto survivor = db.track_by_id(std::stoll(survivorTrackId));
    if (!survivor) {
        throw std::runtime_error("no Engine track with id=" + survivorTrackId);
    }

    // database::remove_track()'s own source comment claims playlist
    // membership is cleared automatically via an "ON DELETE CASCADE"
    // foreign key -- but SQLite only enforces a declared FK constraint
    // when `PRAGMA foreign_keys = ON` is set on the connection, which
    // this library never does anywhere. Confirmed by a real test
    // against a freshly-created database: the cascade did NOT fire,
    // leaving a dangling reference in the playlist. So walk every
    // playlist explicitly first, rather than trust that comment.
    for (auto &root : db.root_playlists()) {
        reassignPlaylistMembership(root, *doomed, *survivor);
    }

    const int64_t doomedId = doomed->id();
    db.remove_track(*doomed);

    // And the track's PerformanceData row, which nothing else deletes.
    //
    // The schema declares it "FOREIGN KEY(trackId) REFERENCES Track(id) ON
    // DELETE CASCADE", and libdjinterop's own track_table::remove() says
    // in a comment that other references "should automatically be cleared"
    // by it -- but SQLite only enforces a declared foreign key when
    // `PRAGMA foreign_keys = ON` is set on the connection, which that
    // library never does anywhere. This is the same mistaken assumption
    // the playlist walk above exists for, and it leaves exactly the same
    // kind of debris: on a real stick, nine rows of waveform, beatgrid and
    // cue data belonging to tracks that no longer existed, reported by
    // `PRAGMA foreign_key_check`.
    //
    // Done through sqlite directly because libdjinterop exposes no way to
    // reach performance data for a track that has just been deleted.
    sqlite3 *raw = nullptr;
    const std::string dbPath = engineDatabaseFile(m_engineLibraryPath);
    if (dbPath.empty() || sqlite3_open_v2(dbPath.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        sqlite3_close(raw);
        // The track itself is gone, which is what the caller asked for;
        // an orphaned row is untidy, not lost data, and is not worth
        // failing a save that has already written to the stick.
        return;
    }
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(raw, "DELETE FROM PerformanceData WHERE trackId = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, doomedId);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(raw);
}

}  // namespace seabass::infrastructure::engine
