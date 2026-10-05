// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <sqlite3.h>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <stdexcept>

#include "infrastructure/compression/zlib_compressor.hpp"
#include "infrastructure/local/local_cue_store.hpp"
#include "infrastructure/paths/seabass_paths.hpp"

#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using namespace seabass::domain;
using namespace seabass::infrastructure::local;
namespace fs = std::filesystem;

namespace
{

// Open file descriptors, where the platform lists them (-1 elsewhere):
// how a connection left open by a constructor that threw is seen.
long openDescriptors()
{
#if defined(__linux__)
    return static_cast<long>(std::distance(fs::directory_iterator("/proc/self/fd"), fs::directory_iterator()));
#else
    return -1;
#endif
}

Track makeTrack(std::string id, std::string filename, std::string title, std::string artist, double duration,
                 std::vector<CuePoint> cues)
{
    Track t;
    t.sourceId = std::move(id);
    t.filename = std::move(filename);
    t.title = std::move(title);
    t.artist = std::move(artist);
    t.durationSeconds = duration;
    t.cues = std::move(cues);
    return t;
}

}  // namespace

int main()
{
    fs::path dbPath = seabass::testing::scratchRoot() / "seabass_local_cue_store_test.db";
    fs::remove(dbPath);

    CuePoint hotCue{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"};

    // Upsert a track with cues, read it back.
    {
        LocalCueStore store(seabass::pathToUtf8(dbPath));
        std::vector<Track> tracks = {
            makeTrack("e1", "song.mp3", "Song", "Artist", 200.0, {hotCue}),
            makeTrack("e2", "no-cues.mp3", "Silent", "Nobody", 100.0, {}),  // no cues -> skipped
        };
        store.upsert(tracks, "engine", "WHALESHARK2");

        auto readBack = store.readAll();
        assert(readBack.size() == 1);  // the no-cues track was never stored
        assert(readBack[0].filename == "song.mp3");
        assert(readBack[0].title == "Song");
        assert(readBack[0].artist == "Artist");
        assert(readBack[0].cues.size() == 1);
        assert(readBack[0].cues[0].kind == CuePoint::Kind::Hot);
        assert(readBack[0].cues[0].positionMs == 1000.0);
        std::cout << "case 1 (upsert stores only tracks with cues, readAll round-trips) OK\n";
    }

    // Re-opening the same database file persists what was written.
    {
        LocalCueStore store(seabass::pathToUtf8(dbPath));
        auto readBack = store.readAll();
        assert(readBack.size() == 1);
        std::cout << "case 2 (data persists across store instances) OK\n";
    }

    // Upserting a track that matches an existing one (by title+artist)
    // replaces its cues rather than adding a second row.
    {
        LocalCueStore store(seabass::pathToUtf8(dbPath));
        CuePoint newCue{CuePoint::Kind::Hot, 2, 5000.0, "#00FF00", "break"};
        std::vector<Track> tracks = {
            makeTrack("e1-rescanned", "song (renamed).mp3", "Song", "Artist", 200.2, {newCue}),
        };
        store.upsert(tracks, "engine", "WHALESHARK2");

        auto readBack = store.readAll();
        assert(readBack.size() == 1);  // still one row, not two
        assert(readBack[0].filename == "song (renamed).mp3");  // metadata updated
        assert(readBack[0].cues.size() == 1);
        assert(readBack[0].cues[0].positionMs == 5000.0);  // cues replaced, not appended
        std::cout << "case 3 (upsert matches by title+artist and replaces cues) OK\n";
    }

    // No filename is no key (#68): two rows with neither title+artist nor
    // a filename, lengths inside the window, are two tracks; one shared
    // filename still finds its row, so the lookup is skipped only when
    // there is nothing to look up by.
    {
        const fs::path noNamePath = seabass::testing::scratchRoot() / "seabass_local_cue_store_noname_test.db";
        fs::remove(noNamePath);
        {
            LocalCueStore store(seabass::pathToUtf8(noNamePath));
            CuePoint second{CuePoint::Kind::Hot, 2, 2000.0, "#00FF00", ""};
            store.upsert({makeTrack("a", "", "", "", 180.0, {hotCue})}, "engine", "STICK");
            store.upsert({makeTrack("b", "", "", "", 180.5, {second})}, "engine", "STICK");
            assert(store.readAll().size() == 2);

            store.upsert({makeTrack("c", "named.mp3", "", "", 240.0, {hotCue})}, "engine", "STICK");
            store.upsert({makeTrack("d", "named.mp3", "", "", 240.5, {second})}, "engine", "STICK");
            assert(store.readAll().size() == 3);
        }
        fs::remove(noNamePath);
        std::cout << "case 3b (an empty filename matches nothing, a shared one still does) OK\n";
    }

    // Snapshots: independent of upsert()'s merged state, each createSnapshot()
    // call freezes its own restorable copy, with an editable description and
    // its own lifecycle (list/read/delete).
    {
        LocalCueStore store(seabass::pathToUtf8(dbPath));
        CuePoint cue2{CuePoint::Kind::Hot, 3, 9000.0, "#0000FF", "outro\twith\ttabs and \\backslash\\"};
        std::vector<Track> tracks = {
            makeTrack("e1", "song.mp3", "Song", "Artist", 200.0, {hotCue, cue2}),
            makeTrack("e2", "no-cues.mp3", "Silent", "Nobody", 100.0, {}),  // no cues -> excluded
        };

        auto id1 = store.createSnapshot(tracks, "engine", "WHALESHARK2", "before Berlin gig");
        auto summaries = store.listSnapshots();
        assert(summaries.size() == 1);
        assert(summaries[0].id == id1);
        assert(summaries[0].description == "before Berlin gig");
        assert(summaries[0].trackCount == 1);  // no-cues track excluded
        assert(summaries[0].cueCount == 2);
        assert(summaries[0].compressedSizeBytes > 0);

        auto restored = store.readSnapshot(id1);
        assert(restored.size() == 1);
        assert(restored[0].title == "Song");
        assert(restored[0].cues.size() == 2);
        assert(restored[0].cues[1].comment == "outro\twith\ttabs and \\backslash\\");  // escaping round-trips
        std::cout << "case 4 (snapshot create/list/read round-trips, including escaped text) OK\n";

        store.setSnapshotDescription(id1, "updated note");
        summaries = store.listSnapshots();
        assert(summaries[0].description == "updated note");
        std::cout << "case 5 (snapshot description is editable) OK\n";

        auto id2 = store.createSnapshot(tracks, "engine", "WHALESHARK2");
        assert(store.listSnapshots().size() == 2);
        assert(store.deleteSnapshot(id2));
        assert(store.listSnapshots().size() == 1);
        assert(!store.deleteSnapshot(id2));  // already gone
        std::cout << "case 6 (snapshots are independently deletable) OK\n";
    }

    // Every snapshot records the format version it was written with, and
    // reading refuses (rather than misparses) a version it doesn't
    // recognize -- the actual backwards-compatibility guarantee: a future
    // format change can never break an old snapshot, since old snapshots
    // simply keep the version number their real format was.
    {
        LocalCueStore store(seabass::pathToUtf8(dbPath));
        std::vector<Track> tracks = {makeTrack("e1", "song.mp3", "Song", "Artist", 200.0, {hotCue})};
        auto id = store.createSnapshot(tracks, "engine", "WHALESHARK2");

        auto summaries = store.listSnapshots();
        assert(summaries[0].id == id);
        assert(summaries[0].schemaVersion == 2);
        std::cout << "case 7 (snapshot records its own format version) OK\n";

        // Simulate a snapshot written by some future seabass version this
        // build doesn't know about.
        sqlite3 *rawDb = nullptr;
        // Two separate reasons this can't be `assert(sqlite3_open(...))`:
        // assert() expands to nothing under NDEBUG, so in a Release build
        // the database was never opened at all (rawDb stayed null and
        // every sqlite3 call below silently did nothing, making this case
        // pass without testing anything); and fs::path::c_str() is
        // const wchar_t* on Windows, which sqlite3_open's const char*
        // parameter refuses -- an error the NDEBUG build never even
        // compiled, so it only surfaced in a Debug build.
        const std::string dbPathUtf8 = seabass::pathToUtf8(dbPath);
        const int openRc = sqlite3_open(dbPathUtf8.c_str(), &rawDb);
        assert(openRc == SQLITE_OK);
        static_cast<void>(openRc);
        sqlite3_stmt *stmt = nullptr;
        sqlite3_prepare_v2(rawDb, "UPDATE backup_sessions SET schema_version = 999 WHERE id = ?", -1, &stmt,
                            nullptr);
        sqlite3_bind_int64(stmt, 1, id);
        const int stepRc = sqlite3_step(stmt);
        assert(stepRc == SQLITE_DONE);
        static_cast<void>(stepRc);
        sqlite3_finalize(stmt);
        sqlite3_close(rawDb);

        bool threw = false;
        try {
            store.readSnapshot(id);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        assert(threw);
        std::cout << "case 8 (unrecognized format version refuses to read rather than misparse) OK\n";
    }

    // Regression test for a real crash: defaultPath() used to read the
    // HOME env var unconditionally, which doesn't exist on Windows --
    // std::getenv("HOME") returned nullptr, and fs::path(nullptr) is UB,
    // crashing every call to the zero-arg LocalCueStore() constructor
    // (e.g. the GUI's "backup to computer" feature) on that platform.
    // Exercising the real, unmocked env here is deliberate: the bug was
    // platform-conditional code never actually running on the platform
    // that needed it, and a fake/injected env wouldn't have caught that.
    {
        std::string path = LocalCueStore::defaultPath();
        assert(!path.empty());
        fs::path p(path);
        assert(p.filename() == "cues.db");
        // Stated against the layout rather than against the literal
        // "~/Seabass/metadata", because the suite runs with SEABASS_HOME
        // pointed at a sandbox so no test can write into the developer's
        // real tree. That the unsandboxed default really is <home>/Seabass
        // is pinned in seabass_paths_test.
        assert(p == seabass::infrastructure::paths::localMetadataDir() / "cues.db");
        assert(p.is_absolute());
        std::cout << "case 9 (defaultPath() resolves to a real, non-empty path) OK\n";
    }

    // Every character the snapshot format escapes, all at once, in every
    // free-text field it writes: tab and newline (the format's own field
    // and line separators), carriage return, and backslash (its escape
    // character) -- including a literal backslash followed by a letter
    // the escape scheme uses, which must come back as those two
    // characters and not as the control character, and one at the very
    // end of a field. Plus a loop, which only format 2 carries.
    {
        const std::string hostile = "a\tb\nc\rd\\e \\t not a tab, \\n not a newline, ends in\\";
        LocalCueStore store(seabass::pathToUtf8(dbPath));
        CuePoint loop{CuePoint::Kind::Memory, 0, 32000.0, hostile, hostile};
        loop.isLoop = true;
        loop.loopEndMs = 36000.0;
        Track track = makeTrack(hostile, hostile + ".mp3", hostile, hostile, 321.5, {loop});
        const auto id = store.createSnapshot({track}, "engine", "WHALESHARK2");

        const auto restored = store.readSnapshot(id);
        assert(restored.size() == 1);
        assert(restored[0].sourceId == hostile);
        assert(restored[0].filename == hostile + ".mp3");
        assert(restored[0].title == hostile);
        assert(restored[0].artist == hostile);
        assert(restored[0].durationSeconds == 321.5);
        assert(restored[0].cues.size() == 1);
        assert(restored[0].cues[0].kind == CuePoint::Kind::Memory);
        assert(restored[0].cues[0].color == hostile);
        assert(restored[0].cues[0].comment == hostile);
        assert(restored[0].cues[0].isLoop);
        assert(restored[0].cues[0].loopEndMs == 36000.0);
        std::cout << "case 10 (every escaped character, in every text field, round-trips) OK\n";
    }

    // A snapshot written in format 1, before loops existed, still reads
    // back through format 1's own parser: planted here as the bytes that
    // format wrote, escapes included, around lines the parser must pass
    // over (a blank one, a cue before any track, short lines, an unknown
    // record type).
    {
        LocalCueStore store(seabass::pathToUtf8(dbPath));
        const auto id = store.createSnapshot(
            {makeTrack("placeholder", "p.mp3", "P", "P", 1.0, {hotCue})}, "rekordbox", "OLD STICK");
        const std::string v1 =
            "C\thot\t1\t10\t\t\n"                               // a cue with no track yet: dropped
            "\n"                                                 // blank
            "T\tv1-id\tback\\\\slash\\ttab.mp3\tOld\\nTitle\tOld Artist\t180.25\n"
            "C\thot\t2\t4500\t#FF8800\tline\\none\\rtwo\n"
            "C\tmemory\t0\t9000.5\t\tplain\n"
            "C\thot\t3\n"                                         // too short: dropped
            "T\tshort\n"                                          // too short: dropped
            "X\tsomething\telse\tentirely\tthat\tno parser\tknows\n";
        const std::string compressed = seabass::infrastructure::compression::compress(v1);

        sqlite3 *rawDb = nullptr;
        const std::string dbPathUtf8 = seabass::pathToUtf8(dbPath);
        const int openRc = sqlite3_open(dbPathUtf8.c_str(), &rawDb);
        assert(openRc == SQLITE_OK);
        static_cast<void>(openRc);
        sqlite3_stmt *stmt = nullptr;
        sqlite3_prepare_v2(rawDb,
                           "UPDATE backup_sessions SET schema_version = 1, data = ?, uncompressed_size_bytes = ? "
                           "WHERE id = ?",
                           -1, &stmt, nullptr);
        sqlite3_bind_blob(stmt, 1, compressed.data(), static_cast<int>(compressed.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(v1.size()));
        sqlite3_bind_int64(stmt, 3, id);
        const int stepRc = sqlite3_step(stmt);
        assert(stepRc == SQLITE_DONE);
        static_cast<void>(stepRc);
        sqlite3_finalize(stmt);
        sqlite3_close(rawDb);

        const auto restored = store.readSnapshot(id);
        assert(restored.size() == 1);
        assert(restored[0].sourceId == "v1-id");
        assert(restored[0].filename == "back\\slash\ttab.mp3");
        assert(restored[0].title == "Old\nTitle");
        assert(restored[0].durationSeconds == 180.25);
        assert(restored[0].cues.size() == 2);
        assert(restored[0].cues[0].kind == CuePoint::Kind::Hot);
        assert(restored[0].cues[0].hotCueNumber == 2);
        assert(restored[0].cues[0].positionMs == 4500.0);
        assert(restored[0].cues[0].color == "#FF8800");
        assert(restored[0].cues[0].comment == "line\none\rtwo");
        assert(!restored[0].cues[0].isLoop);
        assert(restored[0].cues[1].kind == CuePoint::Kind::Memory);
        assert(restored[0].cues[1].positionMs == 9000.5);
        std::cout << "case 11 (a format-1 snapshot reads back through format 1's parser) OK\n";
    }

    // The refusals say what they refuse: an unknown format version names
    // the version, and a session that is not there names its id.
    {
        LocalCueStore store(seabass::pathToUtf8(dbPath));
        const auto summaries = store.listSnapshots();
        std::int64_t future = 0;
        for (const auto &summary : summaries) {
            if (summary.schemaVersion == 999) {
                future = summary.id;
            }
        }
        assert(future != 0);  // case 8's
        std::string message;
        try {
            store.readSnapshot(future);
        } catch (const std::runtime_error &e) {
            message = e.what();
        }
        assert(message.find("format version 999") != std::string::npos);
        assert(message.find("doesn't know how to read") != std::string::npos);

        message.clear();
        try {
            store.readSnapshot(987654);
        } catch (const std::runtime_error &e) {
            message = e.what();
        }
        assert(message == "local cue store: no such backup session 987654");
        std::cout << "case 12 (an unknown format and a missing session are refused by name) OK\n";
    }

    // A store from before snapshots recorded their format, and before
    // cues had loops: opening it adds both columns, and the snapshot it
    // already held reads as format 1, which is what it was written in.
    {
        const fs::path oldPath = dbPath.parent_path() / "seabass_local_cue_store_test_old.db";
        fs::remove(oldPath);
        const std::string v1 = "T\told\told.mp3\tOld\tArtist\t200\nC\thot\t1\t1000\t#FF0000\tdrop\n";
        const std::string compressed = seabass::infrastructure::compression::compress(v1);
        {
            sqlite3 *rawDb = nullptr;
            const std::string oldPathUtf8 = seabass::pathToUtf8(oldPath);
            const int openRc = sqlite3_open(oldPathUtf8.c_str(), &rawDb);
            assert(openRc == SQLITE_OK);
            static_cast<void>(openRc);
            const int createRc = sqlite3_exec(rawDb, R"sql(
                CREATE TABLE tracks (
                    id INTEGER PRIMARY KEY AUTOINCREMENT, filename_normalized TEXT NOT NULL,
                    filename TEXT NOT NULL, title TEXT NOT NULL DEFAULT '', artist TEXT NOT NULL DEFAULT '',
                    title_artist_key TEXT, duration_seconds REAL NOT NULL DEFAULT 0,
                    source_format TEXT NOT NULL, source_label TEXT NOT NULL, backed_up_at TEXT NOT NULL);
                CREATE TABLE cues (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
                    kind TEXT NOT NULL, hot_cue_number INTEGER NOT NULL DEFAULT 0, position_ms REAL NOT NULL,
                    color TEXT NOT NULL DEFAULT '', comment TEXT NOT NULL DEFAULT '');
                CREATE TABLE backup_sessions (
                    id INTEGER PRIMARY KEY AUTOINCREMENT, created_at TEXT NOT NULL, stick_label TEXT NOT NULL,
                    source_format TEXT NOT NULL, description TEXT NOT NULL DEFAULT '',
                    track_count INTEGER NOT NULL, cue_count INTEGER NOT NULL,
                    uncompressed_size_bytes INTEGER NOT NULL, compressed_size_bytes INTEGER NOT NULL,
                    data BLOB NOT NULL);
            )sql", nullptr, nullptr, nullptr);
            assert(createRc == SQLITE_OK);
            static_cast<void>(createRc);
            sqlite3_stmt *stmt = nullptr;
            sqlite3_prepare_v2(rawDb,
                               "INSERT INTO backup_sessions (created_at, stick_label, source_format, track_count, "
                               "cue_count, uncompressed_size_bytes, compressed_size_bytes, data) "
                               "VALUES ('2025-01-01T00:00:00Z', 'OLD', 'engine', 1, 1, ?, ?, ?)",
                               -1, &stmt, nullptr);
            sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(v1.size()));
            sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(compressed.size()));
            sqlite3_bind_blob(stmt, 3, compressed.data(), static_cast<int>(compressed.size()), SQLITE_TRANSIENT);
            const int stepRc = sqlite3_step(stmt);
            assert(stepRc == SQLITE_DONE);
            static_cast<void>(stepRc);
            sqlite3_finalize(stmt);
            sqlite3_close(rawDb);
        }

        // Its own scope: the store holds the database open, and Windows
        // will not delete a file that is open -- fs::remove threw there,
        // uncaught, and the whole test aborted after case 12.
        {
            LocalCueStore store(seabass::pathToUtf8(oldPath));
            const auto summaries = store.listSnapshots();
            assert(summaries.size() == 1);
            assert(summaries[0].schemaVersion == 1);
            const auto restored = store.readSnapshot(summaries[0].id);
            assert(restored.size() == 1 && restored[0].title == "Old" && restored[0].cues.size() == 1);
            assert(restored[0].cues[0].comment == "drop");

            // The loop columns arrived too: a loop upserted now reads back.
            CuePoint loop{CuePoint::Kind::Memory, 0, 2000.0, "", ""};
            loop.isLoop = true;
            loop.loopEndMs = 4000.0;
            store.upsert({makeTrack("n", "new.mp3", "New", "Artist", 100.0, {loop})}, "engine", "OLD");
            const auto all = store.readAll();
            assert(all.size() == 1 && all[0].cues.size() == 1);
            assert(all[0].cues[0].isLoop && all[0].cues[0].loopEndMs == 4000.0);
        }
        fs::remove(oldPath);
        std::cout << "case 13 (a store from before format versions and loops is brought up to date) OK\n";
    }

    // A database this code cannot bring up to date is refused, loudly,
    // with SQLite's own reason, rather than half-opened: here the table a
    // column has to be added to is a view.
    {
        const fs::path oddPath = dbPath.parent_path() / "seabass_local_cue_store_test_odd.db";
        fs::remove(oddPath);
        {
            sqlite3 *rawDb = nullptr;
            const std::string oddPathUtf8 = seabass::pathToUtf8(oddPath);
            const int openRc = sqlite3_open(oddPathUtf8.c_str(), &rawDb);
            assert(openRc == SQLITE_OK);
            static_cast<void>(openRc);
            const int createRc =
                sqlite3_exec(rawDb, "CREATE VIEW backup_sessions AS SELECT 1 AS id;", nullptr, nullptr, nullptr);
            assert(createRc == SQLITE_OK);
            static_cast<void>(createRc);
            sqlite3_close(rawDb);
        }
        std::string message;
        const long fdsBefore = openDescriptors();
        try {
            LocalCueStore store(seabass::pathToUtf8(oddPath));
        } catch (const std::runtime_error &e) {
            message = e.what();
        }
        assert(message.rfind("local cue store: ", 0) == 0);
        assert(message.find("view") != std::string::npos);
        // No destructor runs for a constructor that threw, so the
        // connection it opened has to be closed on the way out.
        assert(openDescriptors() == fdsBefore);
        fs::remove(oddPath);
        std::cout << "case 14 (a schema that cannot be migrated is refused with the reason) OK\n";
    }

    // A path that cannot be a database at all -- a directory -- is
    // refused by the constructor, not discovered by the first query.
    {
        const fs::path dirPath = dbPath.parent_path() / "seabass_local_cue_store_test_dir.db";
        fs::create_directories(dirPath);
        std::string message;
        try {
            LocalCueStore store(seabass::pathToUtf8(dirPath));
        } catch (const std::runtime_error &e) {
            message = e.what();
        }
        assert(message.rfind("local cue store: ", 0) == 0);
        fs::remove_all(dirPath);
        std::cout << "case 15 (a directory in the database's place is refused) OK\n";
    }

    fs::remove(dbPath);
    std::cout << "all cases passed\n";
    return 0;
}
