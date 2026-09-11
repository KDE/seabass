// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include <djinterop/djinterop.hpp>

#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::engine;
namespace fs = std::filesystem;

namespace
{

// Fresh scratch dir per case -- djinterop::engine::create_database()
// refuses to create over an existing database file, so each case needs
// its own directory rather than reusing one across create_database()
// calls.
fs::path freshRoot(const std::string &caseName)
{
    fs::path root =
        seabass::testing::scratchRoot() / "seabass_engine_cue_writer_propagate_test" / caseName / "Engine Library";
    fs::remove_all(root.parent_path());
    fs::create_directories(root.parent_path());
    return root;
}

}  // namespace

int main()
{
    // propagateMissingFields: sets bpm and key on a track that has
    // neither, mirroring what a Clean Up survivor missing both would
    // get from a donor copy elsewhere in its duplicate group.
    {
        fs::path root = freshRoot("case1");
        auto db = djinterop::engine::create_database(root.string());
        djinterop::track_snapshot snapshot;
        snapshot.title = "Survivor";
        snapshot.relative_path = "survivor.mp3";
        auto track = db.create_track(snapshot);
        assert(!track.bpm().has_value());
        assert(!track.key().has_value());

        LibdjinteropEngineCueWriter writer(root.string());
        writer.propagateMissingFields(std::to_string(track.id()), 128.0, std::string("Fm"));

        auto dbAfter = djinterop::engine::load_database(root.string());
        auto after = dbAfter.track_by_id(track.id());
        assert(after.has_value());
        assert(after->bpm().has_value() && *after->bpm() == 128.0);
        assert(after->key().has_value());
        std::cout << "case 1 (propagateMissingFields sets bpm and key) OK\n";
    }

    // Only one of bpm/key passed (the other unset optional) -- only
    // that field is written.
    {
        fs::path root = freshRoot("case2");
        auto db = djinterop::engine::create_database(root.string());
        djinterop::track_snapshot snapshot;
        snapshot.title = "PartialSurvivor";
        snapshot.relative_path = "partial.mp3";
        auto track = db.create_track(snapshot);

        LibdjinteropEngineCueWriter writer(root.string());
        writer.propagateMissingFields(std::to_string(track.id()), std::nullopt, std::string("Am"));

        auto dbAfter = djinterop::engine::load_database(root.string());
        auto after = dbAfter.track_by_id(track.id());
        assert(after.has_value());
        assert(!after->bpm().has_value());
        assert(after->key().has_value());
        std::cout << "case 2 (propagateMissingFields: only the passed field is set) OK\n";
    }

    // Neither field passed -- no-op, no throw.
    {
        fs::path root = freshRoot("case3");
        auto db = djinterop::engine::create_database(root.string());
        djinterop::track_snapshot snapshot;
        snapshot.title = "Untouched";
        snapshot.relative_path = "untouched.mp3";
        auto track = db.create_track(snapshot);

        LibdjinteropEngineCueWriter writer(root.string());
        writer.propagateMissingFields(std::to_string(track.id()), std::nullopt, std::nullopt);

        auto dbAfter = djinterop::engine::load_database(root.string());
        auto after = dbAfter.track_by_id(track.id());
        assert(after.has_value());
        assert(!after->bpm().has_value());
        assert(!after->key().has_value());
        std::cout << "case 3 (propagateMissingFields: neither field -> no-op) OK\n";
    }

    // Unknown track id throws.
    {
        fs::path root = freshRoot("case4");
        auto db = djinterop::engine::create_database(root.string());
        LibdjinteropEngineCueWriter writer(root.string());
        bool threw = false;
        try {
            writer.propagateMissingFields("999999", 120.0, std::nullopt);
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);
        std::cout << "case 4 (propagateMissingFields: unknown track id throws) OK\n";
    }

    std::cout << "All libdjinterop_engine_cue_writer_propagate_test cases passed.\n";
    return 0;
}
