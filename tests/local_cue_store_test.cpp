// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <sqlite3.h>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include "infrastructure/local/local_cue_store.hpp"
#include "infrastructure/paths/seabass_paths.hpp"

#include "scratch_path.hpp"

using namespace seabass::domain;
using namespace seabass::infrastructure::local;
namespace fs = std::filesystem;

namespace
{

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
        LocalCueStore store(dbPath.string());
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
        LocalCueStore store(dbPath.string());
        auto readBack = store.readAll();
        assert(readBack.size() == 1);
        std::cout << "case 2 (data persists across store instances) OK\n";
    }

    // Upserting a track that matches an existing one (by title+artist)
    // replaces its cues rather than adding a second row.
    {
        LocalCueStore store(dbPath.string());
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

    // Snapshots: independent of upsert()'s merged state, each createSnapshot()
    // call freezes its own restorable copy, with an editable description and
    // its own lifecycle (list/read/delete).
    {
        LocalCueStore store(dbPath.string());
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
        LocalCueStore store(dbPath.string());
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
        const std::string dbPathUtf8 = dbPath.string();
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

    fs::remove(dbPath);
    std::cout << "all cases passed\n";
    return 0;
}
