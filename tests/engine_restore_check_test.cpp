// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// checkRestoredEngineLibrary() is what answers "did the restore work"
// once the files have been written back. It is the only thing standing
// between a restore that put the database back without its audio and a
// user being told the stick is fine.
//
// Its three answers are three different things, and only one of them is
// a problem:
//
//   - nullopt: there is no Engine library here, so there was nothing to
//     check. Must never read as "nothing is missing".
//   - an empty list: every local track's file is on the stick.
//   - a non-empty list: those files are not, and the restore is
//     incomplete.
//
// Plus the case that would make this useless on a real library: a
// streaming track has no local file by design, and reporting one as
// missing would fail every restore of a library with Tidal or Beatport
// tracks in it.

#include <cassert>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "infrastructure/engine/engine_library_layout.hpp"
#include "infrastructure/engine/engine_restore_check.hpp"
#include "infrastructure/engine/libdjinterop_engine_library_creator.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"

#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::domain::Track;
using seabass::infrastructure::engine::checkRestoredEngineLibrary;
using seabass::infrastructure::engine::EngineLibraryCreator;
using seabass::infrastructure::engine::engineLibraryPath;
using seabass::infrastructure::engine::engineMainDatabasePath;
using seabass::infrastructure::engine::EngineSchemaGeneration;
using seabass::infrastructure::engine::LibdjinteropEngineReader;

namespace
{

Track makeTrack(const std::string &id, const std::string &title, const fs::path &file)
{
    Track track;
    track.sourceId = id;
    track.title = title;
    track.artist = "An Artist";
    track.filePath = seabass::pathToUtf8(file);
    track.filename = seabass::pathToUtf8(file.filename());
    track.bpm = 128.0;
    track.durationSeconds = 300.0;
    return track;
}

void writeFile(const fs::path &path)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << "not really audio";
}

bool mentions(const std::vector<std::string> &missing, const std::string &needle)
{
    return std::any_of(missing.begin(), missing.end(),
                       [&needle](const std::string &entry) { return entry.find(needle) != std::string::npos; });
}

// Marks a track as streaming, the way a library with a Tidal or Beatport
// subscription in it arrives. The creator has no way to make one, and
// the column is what the reader looks at.
void markStreaming(const fs::path &stickRoot, const std::string &title, const std::string &source)
{
    sqlite3 *db = nullptr;
    assert(sqlite3_open(seabass::pathToUtf8(engineMainDatabasePath(stickRoot)).c_str(), &db) == SQLITE_OK);
    const std::string sql = "UPDATE Track SET streamingSource = '" + source + "' WHERE title = '" + title + "'";
    char *error = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::cout << "could not mark a track as streaming: " << (error ? error : "?") << "\n";
    }
    assert(rc == SQLITE_OK);
    assert(sqlite3_changes(db) == 1);
    sqlite3_free(error);
    sqlite3_close(db);
}

}  // namespace

int main()
{
    const fs::path scratch = seabass::testing::scratchRoot() / "engine_restore_check_test";
    std::error_code ec;
    fs::remove_all(scratch, ec);

    // A target with no Engine library at all. Restoring a stick that
    // never had one is an ordinary thing to do, and it must answer "not
    // applicable" rather than "nothing is missing" -- a caller that
    // cannot tell those apart reports a clean check on a stick this
    // never looked at.
    {
        const fs::path root = scratch / "no-library";
        fs::create_directories(root, ec);
        assert(!checkRestoredEngineLibrary(root).has_value());

        // The folder alone is not a library either: a restore that
        // created the directory tree and then failed leaves exactly
        // this behind.
        fs::create_directories(engineLibraryPath(root) / "Database2", ec);
        assert(!checkRestoredEngineLibrary(root).has_value());
    }

    // A complete restore: every track's file is back on the stick.
    {
        const fs::path root = scratch / "complete";
        const fs::path first = root / "Contents" / "one.mp3";
        const fs::path second = root / "Contents" / "two.mp3";
        writeFile(first);
        writeFile(second);

        const std::vector<Track> tracks{makeTrack("1", "One", first), makeTrack("2", "Two", second)};
        const auto created =
            EngineLibraryCreator::create(seabass::pathToUtf8(engineLibraryPath(root)), tracks, EngineSchemaGeneration::V2);
        assert(created.errorMessage.empty());
        assert(created.tracksCreated == 2);

        const auto missing = checkRestoredEngineLibrary(root);
        assert(missing.has_value());
        if (!missing->empty()) {
            for (const auto &entry : *missing) {
                std::cout << "unexpectedly missing: " << entry << "\n";
            }
        }
        assert(missing->empty());

        // And the check has to be capable of the other answer on this
        // very library, or the empty list above proves nothing: take one
        // file away and it must name it.
        fs::remove(second, ec);
        const auto afterRemoval = checkRestoredEngineLibrary(root);
        assert(afterRemoval.has_value());
        assert(afterRemoval->size() == 1);
        assert(mentions(*afterRemoval, "two.mp3"));
    }

    // The database came back, the audio did not: the failure this
    // whole check exists to catch.
    {
        const fs::path root = scratch / "database-only";
        const fs::path first = root / "Contents" / "one.mp3";
        const fs::path second = root / "Contents" / "two.mp3";
        writeFile(first);
        writeFile(second);

        const std::vector<Track> tracks{makeTrack("1", "One", first), makeTrack("2", "Two", second)};
        const auto created =
            EngineLibraryCreator::create(seabass::pathToUtf8(engineLibraryPath(root)), tracks, EngineSchemaGeneration::V2);
        assert(created.errorMessage.empty());

        fs::remove_all(root / "Contents", ec);
        const auto missing = checkRestoredEngineLibrary(root);
        assert(missing.has_value());
        assert(missing->size() == 2);
        assert(mentions(*missing, "one.mp3"));
        assert(mentions(*missing, "two.mp3"));
    }

    // A streaming track has no file on the stick and never did. Counting
    // it as missing would report every restore of a library with a
    // subscription service in it as incomplete, which is the kind of
    // false alarm that teaches a user to ignore the real one.
    {
        const fs::path root = scratch / "streaming";
        const fs::path local = root / "Contents" / "local.mp3";
        const fs::path streamed = root / "Contents" / "streamed.mp3";
        writeFile(local);
        writeFile(streamed);

        const std::vector<Track> tracks{makeTrack("1", "Local", local), makeTrack("2", "Streamed", streamed)};
        const auto created =
            EngineLibraryCreator::create(seabass::pathToUtf8(engineLibraryPath(root)), tracks, EngineSchemaGeneration::V2);
        assert(created.errorMessage.empty());

        markStreaming(root, "Streamed", "tidal://track/12345");
        fs::remove(streamed, ec);   // a streaming track's "file" is not there, by definition
        fs::remove(local, ec);

        const auto missing = checkRestoredEngineLibrary(root);
        assert(missing.has_value());
        assert(missing->size() == 1);
        assert(mentions(*missing, "local.mp3"));
        assert(!mentions(*missing, "streamed.mp3"));
    }

    fs::remove_all(scratch, ec);
    std::cout << "engine_restore_check_test passed\n";
    return 0;
}
