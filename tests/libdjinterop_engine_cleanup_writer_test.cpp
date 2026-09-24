// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include <djinterop/djinterop.hpp>
#include <sqlite3.h>

#include "infrastructure/engine/libdjinterop_engine_cleanup_writer.hpp"

#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using namespace seabass::infrastructure::engine;
namespace fs = std::filesystem;

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_engine_cleanup_writer_test" / "Engine Library";
    fs::remove_all(root.parent_path());
    fs::create_directories(root.parent_path());

    // A real, fresh Engine database (not a synthetic byte fixture --
    // libdjinterop's own creation API, same trust level as its
    // remove_track()/playlist APIs this class relies on).
    //
    // Playlist "Both" already has both copies -- removing the doomed
    // one should just drop it, no duplicate. Playlist "OnlyDoomed" has
    // only the doomed copy -- the survivor must take over its spot
    // rather than the playlist silently losing the song.
    {
        auto db = djinterop::engine::create_database(seabass::pathToUtf8(root));

        djinterop::track_snapshot snapshot;
        snapshot.title = "Keep Me";
        snapshot.relative_path = "keep.mp3";
        auto survivorTrack = db.create_track(snapshot);

        snapshot.title = "Remove Me";
        snapshot.relative_path = "remove.mp3";
        auto doomedTrack = db.create_track(snapshot);

        auto both = db.create_root_playlist("Both");
        both.add_track_back(survivorTrack);
        both.add_track_back(doomedTrack);

        auto onlyDoomed = db.create_root_playlist("OnlyDoomed");
        onlyDoomed.add_track_back(doomedTrack);

        LibdjinteropEngineCleanupWriter writer(seabass::pathToUtf8(root));
        writer.removeTrackReplacingWith(std::to_string(doomedTrack.id()), std::to_string(survivorTrack.id()));

        // Re-open fresh rather than reusing any in-memory handle -- the
        // point is confirming what's actually on disk now.
        auto dbAfter = djinterop::engine::load_database(seabass::pathToUtf8(root));
        assert(!dbAfter.track_by_id(doomedTrack.id()).has_value());
        assert(dbAfter.track_by_id(survivorTrack.id()).has_value());

        for (const auto &pl : dbAfter.root_playlists()) {
            auto tracks = pl.tracks();
            if (pl.name() == "Both") {
                assert(tracks.size() == 1);
                assert(tracks[0].id() == survivorTrack.id());
            } else if (pl.name() == "OnlyDoomed") {
                assert(tracks.size() == 1);
                assert(tracks[0].id() == survivorTrack.id());  // survivor took over, not left empty
            }
        }
        std::cout << "case 1 (survivor takes over doomed's playlist membership, no duplicates) OK\n";
    }

    // Nested (child) playlists must be handled too, not just root
    // ones -- exercises the recursive walk.
    {
        auto db = djinterop::engine::load_database(seabass::pathToUtf8(root));

        djinterop::track_snapshot snapshot;
        snapshot.title = "Nested Survivor";
        snapshot.relative_path = "nested_survivor.mp3";
        auto survivorTrack = db.create_track(snapshot);
        snapshot.title = "Nested Doomed";
        snapshot.relative_path = "nested_doomed.mp3";
        auto doomedTrack = db.create_track(snapshot);

        auto parentPlaylist = db.create_root_playlist("Parent");
        auto childPlaylist = parentPlaylist.create_sub_playlist("Child");
        childPlaylist.add_track_back(doomedTrack);

        LibdjinteropEngineCleanupWriter writer(seabass::pathToUtf8(root));
        writer.removeTrackReplacingWith(std::to_string(doomedTrack.id()), std::to_string(survivorTrack.id()));

        auto dbAfter = djinterop::engine::load_database(seabass::pathToUtf8(root));
        assert(!dbAfter.track_by_id(doomedTrack.id()).has_value());
        for (const auto &r : dbAfter.root_playlists()) {
            if (r.name() != "Parent") {
                continue;
            }
            for (const auto &child : r.children()) {
                auto tracks = child.tracks();
                assert(tracks.size() == 1);
                assert(tracks[0].id() == survivorTrack.id());
            }
        }
        std::cout << "case 2 (nested/child-playlist membership handled too) OK\n";
    }

    // Not-found cases throw (doomed id, then survivor id).
    {
        auto db = djinterop::engine::load_database(seabass::pathToUtf8(root));
        djinterop::track_snapshot snapshot;
        snapshot.title = "Real Track";
        snapshot.relative_path = "real.mp3";
        auto realTrack = db.create_track(snapshot);

        LibdjinteropEngineCleanupWriter writer(seabass::pathToUtf8(root));
        bool threw = false;
        try {
            writer.removeTrackReplacingWith("999999999", std::to_string(realTrack.id()));
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);

        threw = false;
        try {
            writer.removeTrackReplacingWith(std::to_string(realTrack.id()), "999999999");
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);
        std::cout << "case 3 (nonexistent doomed/survivor id throws) OK\n";
    }

    // Removing a track takes its performance data with it. The schema
    // declares ON DELETE CASCADE, but SQLite only enforces a foreign key
    // when the connection sets `PRAGMA foreign_keys = ON`, which the
    // vendored library never does -- so the row survived its track. A real
    // stick carried nine of them, waveforms and cues belonging to tracks
    // that no longer existed.
    {
        fs::path fourth = root.parent_path() / "cleanup_writer_perfdata";
        fs::remove_all(fourth);
        fs::create_directories(fourth);
        auto db = djinterop::engine::create_database(seabass::pathToUtf8(fourth), djinterop::engine::latest_v2_schema);

        djinterop::track_snapshot doomedSnapshot;
        doomedSnapshot.title = "Doomed";
        doomedSnapshot.relative_path = "doomed.mp3";
        auto doomed = db.create_track(doomedSnapshot);
        djinterop::track_snapshot survivorSnapshot;
        survivorSnapshot.title = "Survivor";
        survivorSnapshot.relative_path = "survivor.mp3";
        auto survivor = db.create_track(survivorSnapshot);
        const std::int64_t doomedId = doomed.id();

        LibdjinteropEngineCleanupWriter writer(seabass::pathToUtf8(fourth));
        writer.removeTrackReplacingWith(std::to_string(doomedId), std::to_string(survivor.id()));

        sqlite3 *raw = nullptr;
        const std::string dbPath = seabass::pathToUtf8(fourth / "Database2" / "m.db");
        assert(sqlite3_open_v2(dbPath.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);

        sqlite3_stmt *stmt = nullptr;
        assert(sqlite3_prepare_v2(raw, "SELECT count(*) FROM PerformanceData WHERE trackId = ?;", -1, &stmt,
                                   nullptr)
               == SQLITE_OK);
        sqlite3_bind_int64(stmt, 1, doomedId);
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        const int leftBehind = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);

        // And the survivor keeps its own, which is the other half: this
        // must delete one row, not every row.
        stmt = nullptr;
        assert(sqlite3_prepare_v2(raw, "SELECT count(*) FROM PerformanceData;", -1, &stmt, nullptr) == SQLITE_OK);
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        const int remaining = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        sqlite3_close(raw);

        assert(leftBehind == 0);
        assert(remaining == 1);
        // Not removed here: db (opened above) still holds Database2/m.db
        // open, and Windows refuses to delete a directory with a file in
        // it still open -- unlike POSIX, which would unlink it happily.
        // The next run's fs::remove_all(root.parent_path()) at the top of
        // main() takes care of it once db has gone out of scope.
        std::cout << "case 4 (the doomed track's performance data goes with it) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
