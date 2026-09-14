// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// An Engine row with no relative path names no file.
//
// The reader used to join the Engine Library folder with the row's
// relative path unconditionally, so every row without one came out as the
// folder itself: one path shared by all of them. Anything that keys on the
// file -- hot cue choices, conflict grouping -- then took unrelated tracks
// for one. The same fallback caught streaming tracks whenever their
// streamingSource could not be read.
//
// Run over a copy of the fixture's Engine library with one row's path
// emptied and another row marked as a TIDAL track.

#include <sqlite3.h>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "application/ports/progress_reporter.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;

namespace
{

struct RecordingReporter : seabass::application::ProgressReporter
{
    std::vector<std::string> warnings;
    void start(const std::string &, size_t) override {}
    void tick(size_t) override {}
    void finish() override {}
    void warn(const std::string &message) override { warnings.push_back(message); }
};

void exec(sqlite3 *db, const std::string &sql)
{
    char *error = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        std::cerr << "sqlite: " << (error ? error : "?") << " in: " << sql << "\n";
        sqlite3_free(error);
        assert(false);
    }
}

}  // namespace

int main()
{
    const fs::path source = fs::path(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "engine";
    const fs::path library = seabass::testing::scratchRoot() / "seabass_engine_reader_paths" / "Engine Library";
    std::error_code ec;
    fs::remove_all(library.parent_path(), ec);
    fs::create_directories(library.parent_path());
    fs::copy(source, library, fs::copy_options::recursive);

    const std::string emptiedId = "1";
    const std::string streamingId = "2";
    {
        sqlite3 *db = nullptr;
        const std::string dbPath = (library / "Database2" / "m.db").string();
        assert(sqlite3_open(dbPath.c_str(), &db) == SQLITE_OK);
        exec(db, "UPDATE Track SET path = '' WHERE id = " + emptiedId);
        exec(db, "UPDATE Track SET streamingSource = 'TIDAL' WHERE id = " + streamingId);
        sqlite3_close(db);
    }

    RecordingReporter reporter;
    seabass::infrastructure::engine::LibdjinteropEngineReader reader(library.string());
    reader.setProgressReporter(reporter);
    const auto tracks = reader.readAll();
    assert(!tracks.empty());

    const std::string folder = fs::path(library).lexically_normal().string();
    bool sawEmptied = false;
    bool sawStreaming = false;
    for (const auto &track : tracks) {
        const std::string asDirectory = fs::path(track.filePath).lexically_normal().string();
        if (!track.filePath.empty() && (asDirectory == folder || asDirectory == folder + "/")) {
            std::cerr << "track " << track.sourceId << " has the Engine Library folder as its file: " << track.filePath
                      << "\n";
            assert(false && "no track's file is the library folder itself");
        }
        if (track.sourceId == emptiedId) {
            sawEmptied = true;
            assert(track.filePath.empty() && "no relative path names no file");
        }
        if (track.sourceId == streamingId) {
            sawStreaming = true;
            assert(track.streamingSource == "TIDAL");
        }
    }
    assert(sawEmptied && sawStreaming);
    for (const auto &warning : reporter.warnings) {
        assert(warning.find("streaming sources") == std::string::npos
               && "streaming sources are readable here; the warning is for when they are not");
    }
    std::cout << "case 1 (an empty relative path gives no file, and streaming sources are read) OK\n";

    fs::remove_all(library.parent_path(), ec);
    std::cout << "engine_reader_paths_test passed\n";
    return 0;
}
