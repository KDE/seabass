// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include <djinterop/djinterop.hpp>

#include "infrastructure/engine/libdjinterop_engine_cleanup_writer.hpp"

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
        auto db = djinterop::engine::create_database(root.string());

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

        LibdjinteropEngineCleanupWriter writer(root.string());
        writer.removeTrackReplacingWith(std::to_string(doomedTrack.id()), std::to_string(survivorTrack.id()));

        // Re-open fresh rather than reusing any in-memory handle -- the
        // point is confirming what's actually on disk now.
        auto dbAfter = djinterop::engine::load_database(root.string());
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
        auto db = djinterop::engine::load_database(root.string());

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

        LibdjinteropEngineCleanupWriter writer(root.string());
        writer.removeTrackReplacingWith(std::to_string(doomedTrack.id()), std::to_string(survivorTrack.id()));

        auto dbAfter = djinterop::engine::load_database(root.string());
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
        auto db = djinterop::engine::load_database(root.string());
        djinterop::track_snapshot snapshot;
        snapshot.title = "Real Track";
        snapshot.relative_path = "real.mp3";
        auto realTrack = db.create_track(snapshot);

        LibdjinteropEngineCleanupWriter writer(root.string());
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

    std::cout << "all cases passed\n";
    return 0;
}
