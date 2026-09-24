// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "domain/track.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"

#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/sqlite_pending_journal.hpp"
#include "scratch_path.hpp"

using namespace seabass::infrastructure::onelibrary;
using namespace seabass::domain;
namespace fs = std::filesystem;

namespace
{

// Fresh empty scratch dir, ready for one test case -- same convention
// onelibrary_cue_writer_test.cpp uses.
fs::path freshScratch()
{
    fs::path scratch = seabass::testing::scratchRoot() / "seabass_onelibrary_reader_test";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    return scratch;
}

// Builds a minimal-but-real exportLibrary.db fixture covering every table
// and column OneLibraryReader::readAll() actually queries (content, artist,
// key, image, playlist, playlist_content, cue) -- column names/types match
// a real stick's `.schema` output (see docs/onelibrary-format.md), trimmed
// to what this test exercises. Three tracks, exercising the three
// LEFT JOIN outcomes readAll() has to handle: a fully-populated track, a
// track whose image row points at a file that doesn't exist on disk, and a
// track with no image row at all.
void createFixture(const std::string &pioneerRoot)
{
    fs::create_directories(seabass::pathFromUtf8(pioneerRoot) / "rekordbox");
    std::string dbPath = OneLibraryCueWriter::dbPathFor(pioneerRoot);

    std::string key = deriveOneLibraryKey();
    SqlCipherLibrary lib;
    SqlCipherDb db(lib, dbPath, /*readOnly=*/false);
    db.exec("PRAGMA key = '" + key + "';");

    db.exec("CREATE TABLE artist(artist_id integer primary key, name varchar);");
    db.exec("CREATE TABLE key(key_id integer primary key, name varchar);");
    db.exec("CREATE TABLE image(image_id integer primary key, path varchar);");
    db.exec("CREATE TABLE album(album_id integer primary key, name varchar);");
    db.exec(
        "CREATE TABLE content(content_id integer primary key, title varchar, artist_id_artist integer, "
        "bpmx100 integer, length integer, path varchar, fileName varchar, bitrate integer, fileSize integer, "
        "key_id integer, djPlayCount integer, image_id integer, album_id integer);");
    db.exec("CREATE TABLE playlist(playlist_id integer primary key, name varchar, playlist_id_parent integer);");
    db.exec("CREATE TABLE playlist_content(content_id integer, playlist_id integer, sequenceNo integer);");
    db.exec("CREATE TABLE cue(content_id integer, kind integer, inUsec integer, cueComment varchar, isActiveLoop integer, outUsec integer);");

    db.exec("INSERT INTO artist VALUES (1, 'Test Artist');");
    db.exec("INSERT INTO album VALUES (1, 'Test Album');");
    db.exec("INSERT INTO key VALUES (1, 'Fm');");
    // image_id 1 resolves to a real file (created below); image_id 2's
    // path is never created on disk -- readAll() must leave artworkPath
    // empty rather than pointing at a non-existent file.
    db.exec("INSERT INTO image VALUES (1, '/PIONEER/Artwork/00001/a1.jpg');");
    db.exec("INSERT INTO image VALUES (2, '/PIONEER/Artwork/00002/missing.jpg');");

    db.exec(
        "INSERT INTO content (content_id, title, artist_id_artist, bpmx100, length, path, fileName, bitrate, "
        "fileSize, key_id, djPlayCount, image_id, album_id) VALUES "
        "(566, 'Test Track', 1, 12800, 245, '/Contents/Test Track.mp3', 'Test Track.mp3', 320, 654321, 1, 5, 1, 1);");
    db.exec(
        "INSERT INTO content (content_id, title, artist_id_artist, bpmx100, length, path, fileName, bitrate, "
        "fileSize, key_id, djPlayCount, image_id) VALUES "
        "(2, 'No Art Track', NULL, 0, 0, '/Contents/No Art.mp3', 'No Art.mp3', 0, 0, NULL, NULL, 2);");
    db.exec(
        "INSERT INTO content (content_id, title, artist_id_artist, bpmx100, length, path, fileName, bitrate, "
        "fileSize, key_id, djPlayCount, image_id) VALUES "
        "(3, 'No Image Row Track', NULL, 0, 0, '/Contents/No Image.mp3', 'No Image.mp3', 0, 0, NULL, NULL, NULL);");

    db.exec("INSERT INTO playlist VALUES (10, 'Techno', 0);");
    db.exec("INSERT INTO playlist VALUES (11, 'Peak Time', 10);");
    db.exec("INSERT INTO playlist_content VALUES (566, 11, 3);");

    db.exec("INSERT INTO cue (content_id, kind, inUsec, cueComment) VALUES (566, 0, 12345000, 'breakdown');");   // memory cue
    db.exec("INSERT INTO cue (content_id, kind, inUsec, cueComment) VALUES (566, 1, 1000000, 'drop');");        // hot cue slot 1
    db.exec("INSERT INTO cue (content_id, kind, inUsec, cueComment, isActiveLoop, outUsec) "
            "VALUES (566, 2, 2000000, 'loop', 1, 4000000);");  // hot loop in slot 2, 2.0 s to 4.0 s
}

const Track *findBySourceId(const std::vector<Track> &tracks, const std::string &sourceId)
{
    for (const auto &t : tracks) {
        if (t.sourceId == sourceId) {
            return &t;
        }
    }
    return nullptr;
}

}  // namespace

int main()
{
    // Case 1: a fully-populated track parses every joined field correctly,
    // including a real, resolvable artwork file.
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(seabass::pathToUtf8(pioneerRoot));

        fs::path artFile = scratch / "PIONEER" / "Artwork" / "00001" / "a1.jpg";
        fs::create_directories(artFile.parent_path());
        std::ofstream(artFile) << "fake jpeg bytes";

        OneLibraryReader reader(seabass::pathToUtf8(pioneerRoot));
        std::vector<Track> tracks = reader.readAll();
        assert(tracks.size() == 3);

        const Track *t = findBySourceId(tracks, "566");
        assert(t != nullptr);
        assert(t->format == "onelibrary");
        assert(t->title == "Test Track");
        assert(t->artist == "Test Artist");
        // Resolved through the album table, the same shape as artist.
        assert(t->album == "Test Album");
        assert(t->bpm == 128.0);
        assert(t->durationSeconds == 245.0);
        assert(t->filePath == seabass::pathToUtf8(scratch / "Contents" / "Test Track.mp3"));
        assert(t->key == "Fm");
        assert(t->bitrate == 320);
        assert(t->fileSizeBytes == 654321);
        assert(t->playCount.has_value() && *t->playCount == 5);
        assert(t->artworkPath == seabass::pathToUtf8(artFile));

        assert(t->cues.size() == 3);
        bool sawHot = false, sawMemory = false, sawLoop = false;
        for (const auto &c : t->cues) {
            if (c.kind == CuePoint::Kind::Hot && c.hotCueNumber == 2) {
                // A loop row: flagged, and its out point carried.
                assert(c.isLoop && c.loopEndMs == 4000.0 && c.positionMs == 2000.0);
                sawLoop = true;
            } else if (c.kind == CuePoint::Kind::Hot) {
                assert(c.hotCueNumber == 1);
                assert(c.positionMs == 1000.0);  // 1,000,000us -> 1000.0ms
                assert(c.comment == "drop");
                assert(!c.isLoop);
                sawHot = true;
            } else {
                assert(c.hotCueNumber == 0);
                assert(c.positionMs == 12345.0);  // 12,345,000us -> 12345.0ms
                assert(c.comment == "breakdown");
                sawMemory = true;
            }
        }
        assert(sawHot && sawMemory && sawLoop);

        assert(t->playlists.size() == 1);
        assert(t->playlists[0].name == "Techno/Peak Time");
        assert(t->playlists[0].position == 3);

        std::cout << "case 1 (fully-populated track parses every joined field) OK\n";
    }

    // Case 2: an image row exists but the file it points to doesn't --
    // artworkPath must stay empty rather than pointing at a missing file.
    // Also: NULL artist/key/djPlayCount all degrade to empty/unset rather
    // than a garbage value or a crash. No cues, no playlists.
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(seabass::pathToUtf8(pioneerRoot));
        // Deliberately not creating the file image_id 2 points at.

        OneLibraryReader reader(seabass::pathToUtf8(pioneerRoot));
        std::vector<Track> tracks = reader.readAll();

        const Track *t = findBySourceId(tracks, "2");
        assert(t != nullptr);
        assert(t->artist.empty());
        assert(t->key.empty());
        assert(!t->playCount.has_value());
        assert(t->artworkPath.empty());
        assert(t->cues.empty());
        assert(t->playlists.empty());

        std::cout << "case 2 (image row with a missing file leaves artworkPath empty) OK\n";
    }

    // Case 3: no image row at all (image_id NULL) -- same empty-artworkPath
    // outcome as case 2, but via the LEFT JOIN producing no row rather than
    // a row whose file is missing.
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        createFixture(seabass::pathToUtf8(pioneerRoot));

        OneLibraryReader reader(seabass::pathToUtf8(pioneerRoot));
        std::vector<Track> tracks = reader.readAll();

        const Track *t = findBySourceId(tracks, "3");
        assert(t != nullptr);
        assert(t->artworkPath.empty());

        std::cout << "case 3 (no image row at all also leaves artworkPath empty) OK\n";
    }

    // Case 4: no exportLibrary.db present for this stick at all -- readAll()
    // must throw rather than silently return an empty list (callers rely on
    // this to distinguish "no OneLibrary here" from "empty OneLibrary").
    {
        fs::path scratch = freshScratch();
        fs::path pioneerRoot = scratch / "PIONEER";
        fs::create_directories(pioneerRoot);  // PIONEER exists, but no rekordbox/exportLibrary.db under it

        OneLibraryReader reader(seabass::pathToUtf8(pioneerRoot));
        bool threw = false;
        try {
            reader.readAll();
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);

        std::cout << "case 4 (missing exportLibrary.db throws rather than returning empty) OK\n";
    }

    // Case 5: a stick pulled mid-save (#48). OneLibrary is left with a hot
    // journal and changed pages; a read-only open used to fail with
    // "attempt to write a readonly database", which is what every page
    // reading it showed in the macOS round 8 check P3. readAll() now rolls
    // it back first -- keeping a copy on this computer -- and reads the
    // library as it was before the unfinished transaction.
    {
        fs::path scratch = freshScratch();
        // Where the kept copy goes: this test's scratch, never ~/Seabass.
        const std::string home = seabass::pathToUtf8(scratch / "home");
#ifdef _WIN32
        _putenv_s("SEABASS_HOME", home.c_str());
#else
        setenv("SEABASS_HOME", home.c_str(), 1);
#endif
        fs::path live = scratch / "live" / "PIONEER";
        createFixture(seabass::pathToUtf8(live));
        const std::string liveDb = OneLibraryCueWriter::dbPathFor(seabass::pathToUtf8(live));

        fs::path pulled = scratch / "pulled" / "PIONEER";
        fs::create_directories(pulled / "rekordbox");
        const std::string pulledDb = OneLibraryCueWriter::dbPathFor(seabass::pathToUtf8(pulled));
        {
            SqlCipherLibrary lib;
            SqlCipherDb db(lib, liveDb, /*readOnly=*/false);
            db.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
            db.exec("PRAGMA journal_mode=DELETE;");
            // Spill after one page, so the transaction reaches the file
            // and the journal gets its live header before any commit.
            db.exec("PRAGMA cache_size=1;");
            db.exec("PRAGMA cache_spill=1;");
            db.exec("BEGIN;");
            db.exec("UPDATE content SET title = 'HALF WRITTEN';");
            db.exec("CREATE TABLE filler(x TEXT);");
            db.exec("WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM c WHERE i < 2000) "
                    "INSERT INTO filler SELECT hex(randomblob(300)) FROM c;");
            fs::copy_file(seabass::pathFromUtf8(liveDb), seabass::pathFromUtf8(pulledDb));
            fs::copy_file(seabass::pathFromUtf8(liveDb + "-journal"), seabass::pathFromUtf8(pulledDb + "-journal"));
            db.exec("ROLLBACK;");
        }
        assert(seabass::infrastructure::hasPendingJournal(seabass::pathFromUtf8(pulledDb))
               && "the copy is a stick pulled mid-save");

        OneLibraryReader reader(seabass::pathToUtf8(pulled));
        std::vector<Track> tracks = reader.readAll();
        assert(tracks.size() == 3 && "the library reads again");
        const Track *t = findBySourceId(tracks, "566");
        assert(t != nullptr && t->title == "Test Track" && "and reads as it was before the unfinished save");
        assert(!seabass::infrastructure::hasPendingJournal(seabass::pathFromUtf8(pulledDb)));
        bool kept = false;
        std::error_code ec;
        for (const auto &entry : fs::recursive_directory_iterator(scratch / "home" / "recovered", ec)) {
            if (entry.path().filename() == "exportLibrary.db-journal") {
                kept = true;
            }
        }
        assert(kept && "a copy of the database and its journal was kept first");
        std::cout << "case 5 (a stick pulled mid-save reads again, as it was) OK\n";
    }

    std::cout << "All onelibrary_reader_test cases passed.\n";
    return 0;
}
