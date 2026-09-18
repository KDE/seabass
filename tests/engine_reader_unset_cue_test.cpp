// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A track with nothing cued has no cues.
//
// Engine stores "no main cue" as a sample offset of -1, and the reader
// divided that by the sample rate like any other offset, giving every
// un-cued track a memory cue a fraction of a millisecond before the
// track starts. One real library carried 958 of them, and they travelled
// wherever cues travel: metadata backups, restore offers, cue counts,
// duplicate comparisons.

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

#include <djinterop/djinterop.hpp>

#include "domain/track.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"

#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;

int main()
{
    const fs::path root = testing::scratchRoot() / "seabass_engine_reader_unset_cue_test" / "Engine Library";
    fs::remove_all(root.parent_path());
    fs::create_directories(root.parent_path());

    {
        auto db = djinterop::engine::create_database(root.string());

        djinterop::track_snapshot bare;
        bare.title = "Never Cued";
        bare.relative_path = "../Contents/never-cued.mp3";
        db.create_track(bare);

        djinterop::track_snapshot cued;
        cued.title = "Has A Cue";
        cued.relative_path = "../Contents/has-a-cue.mp3";
        cued.sample_rate = 44100.0;
        cued.main_cue = 44100.0;  // one second in
        db.create_track(cued);
    }

    infrastructure::engine::LibdjinteropEngineReader reader(root.string());
    const std::vector<domain::Track> tracks = reader.readAll();
    assert(tracks.size() == 2);

    for (const domain::Track &track : tracks) {
        if (track.title == "Never Cued") {
            assert(track.cues.empty() && "an unset main cue is not a cue at minus one sample");
        } else {
            assert(track.cues.size() == 1);
            assert(track.cues[0].kind == domain::CuePoint::Kind::Memory);
            assert(track.cues[0].positionMs > 999.0 && track.cues[0].positionMs < 1001.0);
        }
    }

    fs::remove_all(root.parent_path());
    std::cout << "engine_reader_unset_cue_test: all cases passed\n";
    return 0;
}
