// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/libdjinterop_engine_cleanup_writer.hpp"

#include <stdexcept>

#include <djinterop/djinterop.hpp>

namespace seabass::infrastructure::engine
{

namespace
{

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

    db.remove_track(*doomed);
}

}  // namespace seabass::infrastructure::engine
