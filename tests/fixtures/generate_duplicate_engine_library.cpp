// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// One-off generator for tests/fixtures/duplicate_engine_library/ -- a small
// synthetic Engine Library with one track duplicated (same filename,
// duration): one copy has hot cues, the other doesn't. Used to exercise the
// duplicate-cue consolidation write path end to end. Not built by the main
// CMake project; run manually if the fixture ever needs regenerating:
//
//   g++ -std=c++20 -I third_party/libdjinterop/include \
//       -I build/third_party/libdjinterop/include \
//       tests/fixtures/generate_duplicate_engine_library.cpp \
//       build/third_party/libdjinterop/libdjinterop.a -lsqlite3 -lz \
//       -o /tmp/gen && /tmp/gen

#include <djinterop/djinterop.hpp>

int main()
{
    namespace e = djinterop::engine;
    bool created;
    auto db = e::create_or_load_database("tests/fixtures/duplicate_engine_library", e::latest_schema, created);

    for (auto &tr : db.tracks()) {
        db.remove_track(tr);
    }

    djinterop::track_snapshot withCues;
    withCues.relative_path = "../folderA/Duplicate Song.mp3";
    withCues.title = "Duplicate Song";
    withCues.artist = "Test Artist";
    withCues.duration = std::chrono::milliseconds{200000};
    withCues.sample_rate = 44100;
    withCues.sample_count = 8820000;
    withCues.hot_cues.resize(8);
    withCues.hot_cues[0] = djinterop::hot_cue{"Intro", 44100.0, e::standard_pad_colors::pad_1};
    db.create_track(withCues);

    djinterop::track_snapshot withoutCues;
    withoutCues.relative_path = "../folderB/Duplicate Song.mp3";
    withoutCues.title = "Duplicate Song";
    withoutCues.artist = "Test Artist";
    withoutCues.duration = std::chrono::milliseconds{200000};
    withoutCues.sample_rate = 44100;
    withoutCues.sample_count = 8820000;
    db.create_track(withoutCues);

    return 0;
}
