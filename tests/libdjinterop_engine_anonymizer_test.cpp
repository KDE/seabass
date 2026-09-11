// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

#include <djinterop/djinterop.hpp>

#include "infrastructure/engine/libdjinterop_engine_anonymizer.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::engine;
namespace fs = std::filesystem;

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_engine_anonymizer_test";
    fs::remove_all(root);
    fs::create_directories(root);

    fs::path sourceRoot = root / "source" / "Engine Library";
    fs::path destRoot = root / "dest" / "Engine Library";
    fs::create_directories(sourceRoot.parent_path());

    int64_t track1Id, track2Id, track3Id;
    {
        // A real, fresh Engine database -- same trust level as the
        // rest of this codebase's Engine-side tests. Tracks 1 and 2
        // share a real artist; track 3 has a different one.
        auto db = djinterop::engine::create_database(sourceRoot.string());

        djinterop::track_snapshot snapshot;
        snapshot.title = "Real Title 1";
        snapshot.artist = "Real Artist A";
        snapshot.comment = "Real Comment 1";
        snapshot.relative_path = "Contents/real1.mp3";
        auto track1 = db.create_track(snapshot);
        track1Id = track1.id();

        // A real hot cue label -- free text a DJ typed per cue point,
        // the Engine-side equivalent of a rekordbox cue comment.
        djinterop::hot_cue cue;
        cue.label = "Real Drop Cue";
        cue.sample_offset = 44100.0;
        track1.set_hot_cue_at(0, cue);

        snapshot.title = "Real Title 2";
        snapshot.artist = "Real Artist A";
        snapshot.comment = "Real Comment 2";
        snapshot.relative_path = "Contents/real2.mp3";
        auto track2 = db.create_track(snapshot);
        track2Id = track2.id();

        snapshot.title = "Real Title 3";
        snapshot.artist = "Real Artist B";
        snapshot.comment = "Real Comment 3";
        snapshot.relative_path = "Contents/real3.mp3";
        auto track3 = db.create_track(snapshot);
        track3Id = track3.id();

        auto root_pl = db.create_root_playlist("Real Playlist");
        root_pl.add_track_back(track1);
        root_pl.add_track_back(track2);
        root_pl.add_track_back(track3);
        auto nested = root_pl.create_sub_playlist("Real Nested Playlist");
        nested.add_track_back(track3);
    }

    auto result = anonymizeEngineLibrary(sourceRoot.string(), destRoot.string(), 2);

    assert(result.errorMessage.empty());
    assert(result.tracksKept == 2);
    assert(result.tracksDropped == 1);
    assert(result.playlistsRenamed == 2);  // root + nested
    std::cout << "case 1 (anonymizeEngineLibrary: prune/rename counts correct) OK\n";

    auto dbAfter = djinterop::engine::load_database(destRoot.string());
    assert(dbAfter.track_by_id(track1Id).has_value());
    assert(dbAfter.track_by_id(track2Id).has_value());
    assert(!dbAfter.track_by_id(track3Id).has_value());  // pruned
    std::cout << "case 2 (kept tracks are exactly the first maxTracks, pruned track gone) OK\n";

    auto t1 = *dbAfter.track_by_id(track1Id);
    auto t2 = *dbAfter.track_by_id(track2Id);
    assert(t1.title() != std::optional<std::string>("Real Title 1"));
    assert(t2.title() != std::optional<std::string>("Real Title 2"));
    assert(t1.title() != t2.title());  // distinct per-track placeholders
    assert(t1.comment() != std::optional<std::string>("Real Comment 1"));
    std::cout << "case 3 (title/comment obfuscated and distinct per track) OK\n";

    assert(t1.artist() == t2.artist());  // shared real artist -> same placeholder
    assert(t1.artist() != std::optional<std::string>("Real Artist A"));
    std::cout << "case 4 (tracks sharing a real artist still share their placeholder artist) OK\n";

    for (const auto &pl : dbAfter.root_playlists()) {
        assert(pl.name() != "Real Playlist");  // renamed
        auto tracks = pl.tracks();
        bool has1 = false, has2 = false, has3 = false;
        for (const auto &t : tracks) {
            if (t.id() == track1Id) has1 = true;
            if (t.id() == track2Id) has2 = true;
            if (t.id() == track3Id) has3 = true;
        }
        assert(has1 && has2);
        assert(!has3);  // pruned track removed from playlist membership too
        for (const auto &child : pl.children()) {
            assert(child.name() != "Real Nested Playlist");  // nested playlist renamed too
        }
    }
    std::cout << "case 5 (playlist membership for the pruned track removed; names renamed, incl. nested) OK\n";

    auto keptCue = t1.hot_cue_at(0);
    assert(keptCue.has_value());
    assert(keptCue->label != "Real Drop Cue");
    assert(!keptCue->label.empty());
    assert(keptCue->sample_offset == 44100.0);  // position untouched, only the label changed
    std::cout << "case 6 (real hot cue label is obfuscated, not left verbatim; position untouched) OK\n";

    // Source untouched -- every edit happens on the destination copy.
    auto dbSource = djinterop::engine::load_database(sourceRoot.string());
    auto sourceTrack1 = *dbSource.track_by_id(track1Id);
    assert(sourceTrack1.title() == std::optional<std::string>("Real Title 1"));
    assert(sourceTrack1.hot_cue_at(0)->label == "Real Drop Cue");
    std::cout << "case 7 (source library untouched) OK\n";

    std::cout << "all cases passed\n";
    return 0;
}
