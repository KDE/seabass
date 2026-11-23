// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include <sqlite3.h>

#include <djinterop/djinterop.hpp>

#include "infrastructure/engine/engine_artwork.hpp"
#include "infrastructure/engine/libdjinterop_engine_anonymizer.hpp"

#include "infrastructure/paths/utf8_path.hpp"
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
        auto db = djinterop::engine::create_database(seabass::pathToUtf8(sourceRoot));

        djinterop::track_snapshot snapshot;
        snapshot.title = "Real Title 1";
        snapshot.artist = "Real Artist A";
        snapshot.comment = "Real Comment 1";
        // What a real stick holds: out of Engine Library, then down
        // into the stick's own Contents, beside real artist and album
        // folders. The test used to spell every path the way only an
        // export does, which is why nothing here noticed that the
        // hops were being dropped (seabass#13).
        snapshot.relative_path = "../Contents/Real Artist A/Real Album/real1.mp3";
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
        snapshot.relative_path = "../Contents/Real Artist A/Real Album/real2.mp3";
        auto track2 = db.create_track(snapshot);
        track2Id = track2.id();

        snapshot.title = "Real Title 3";
        snapshot.artist = "Real Artist B";
        snapshot.comment = "Real Comment 3";
        // Deliberately hopless: a library whose paths are already
        // relative to the same directory must not gain hops it never
        // had.
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

    // A cover kept inside the database, the way older libraries store
    // them: an image is as identifying as a title.
    const std::string coverMarker = "A-REAL-COVER-KEPT-IN-THE-ROW";
    {
        sqlite3 *handle = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(sourceRoot / "Database2" / "m.db").c_str(), &handle) == SQLITE_OK);
        const std::string image = std::string("\xFF\xD8\xFF", 3) + coverMarker;
        sqlite3_stmt *insert = nullptr;
        assert(sqlite3_prepare_v2(handle,
                                  "INSERT INTO AlbumArt (id, hash, albumArt) VALUES "
                                  "(7, '551c96558e2eb05ea31f3735b129f242b720c15', ?);",
                                  -1, &insert, nullptr)
               == SQLITE_OK);
        sqlite3_bind_blob(insert, 1, image.data(), static_cast<int>(image.size()), SQLITE_TRANSIENT);
        assert(sqlite3_step(insert) == SQLITE_DONE);
        sqlite3_finalize(insert);
        assert(sqlite3_exec(handle, ("UPDATE Track SET albumArtId = 7 WHERE id = " + std::to_string(track1Id)).c_str(),
                            nullptr, nullptr, nullptr)
               == SQLITE_OK);
        // A file-kind row with an empty value in albumArt: nothing to take
        // out, and its hash names a file, so it stays.
        assert(sqlite3_exec(handle,
                            "INSERT INTO AlbumArt (id, hash, albumArt) VALUES "
                            "(8, x'1111111111111111111111111111111111111111', x'');",
                            nullptr, nullptr, nullptr)
               == SQLITE_OK);
        sqlite3_close(handle);
    }

    auto result = anonymizeEngineLibrary(seabass::pathToUtf8(sourceRoot), seabass::pathToUtf8(destRoot));

    assert(result.errorMessage.empty());
    assert(result.tracksAnonymized == 3);
    assert(result.playlistsRenamed == 2);  // root + nested
    std::cout << "case 1 (anonymizeEngineLibrary: rename counts correct) OK\n";

    auto dbAfter = djinterop::engine::load_database(seabass::pathToUtf8(destRoot));
    assert(dbAfter.track_by_id(track1Id).has_value());
    assert(dbAfter.track_by_id(track2Id).has_value());
    assert(dbAfter.track_by_id(track3Id).has_value());
    std::cout << "case 2 (every track is kept) OK\n";

    auto t1 = *dbAfter.track_by_id(track1Id);
    auto t2 = *dbAfter.track_by_id(track2Id);
    assert(t1.title() != std::optional<std::string>("Real Title 1"));
    assert(t2.title() != std::optional<std::string>("Real Title 2"));
    assert(t1.title() != t2.title());  // distinct per-track placeholders
    assert(t1.comment() != std::optional<std::string>("Real Comment 1"));
    // The last track too, so a pass that stopped short of the end could
    // not pass this.
    auto t3 = *dbAfter.track_by_id(track3Id);
    assert(t3.title() != std::optional<std::string>("Real Title 3"));
    assert(t3.comment() != std::optional<std::string>("Real Comment 3"));
    assert(t3.artist() != std::optional<std::string>("Real Artist B"));
    assert(t3.title() != t2.title());
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
        assert(has1 && has2 && has3);
        for (const auto &child : pl.children()) {
            assert(child.name() != "Real Nested Playlist");  // nested playlist renamed too
        }
    }
    std::cout << "case 5 (playlist membership kept; names renamed, incl. nested) OK\n";

    auto keptCue = t1.hot_cue_at(0);
    assert(keptCue.has_value());
    assert(keptCue->label != "Real Drop Cue");
    assert(!keptCue->label.empty());
    assert(keptCue->sample_offset == 44100.0);  // position untouched, only the label changed
    std::cout << "case 6 (real hot cue label is obfuscated, not left verbatim; position untouched) OK\n";

    // Where the audio is, relative to the catalog, has to survive: the
    // two catalogs of one stick describe ONE set of files, and the path
    // is the matcher's most reliable signal for saying so. The real
    // artist and album folders are gone; the hops out of Engine Library
    // are not.
    assert(t1.relative_path().rfind("../Contents/", 0) == 0);
    assert(t2.relative_path().rfind("../Contents/", 0) == 0);
    assert(t1.relative_path().find("Real Artist A") == std::string::npos);
    assert(t1.relative_path().find("Real Album") == std::string::npos);
    assert(t1.relative_path().find("real1.mp3") == std::string::npos);
    // And a path that never had hops does not acquire any.
    assert(t3.relative_path().rfind("Contents/", 0) == 0);
    assert(t3.relative_path().find("..") == std::string::npos);
    std::cout << "case 7 (the path keeps its hops out of Engine Library, loses the real folders) OK\n";

    // Source untouched -- every edit happens on the destination copy.
    auto dbSource = djinterop::engine::load_database(seabass::pathToUtf8(sourceRoot));
    auto sourceTrack1 = *dbSource.track_by_id(track1Id);
    assert(sourceTrack1.title() == std::optional<std::string>("Real Title 1"));
    assert(sourceTrack1.hot_cue_at(0)->label == "Real Drop Cue");
    std::cout << "case 8 (source library untouched) OK\n";

    // The cover is gone from the export, and not merely from the live row:
    // the file itself no longer holds its bytes. The row stays, so the
    // track still points where it did.
    {
        assert(result.albumArtImagesRemoved == 1);  // not the empty value beside a file hash
        const fs::path exported = destRoot / "Database2" / "m.db";
        sqlite3 *handle = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(exported).c_str(), &handle) == SQLITE_OK);
        sqlite3_stmt *stmt = nullptr;
        assert(sqlite3_prepare_v2(handle, "SELECT a.id, a.albumArt IS NULL FROM Track t JOIN AlbumArt a "
                                          "ON a.id = t.albumArtId WHERE t.id = ?;",
                                  -1, &stmt, nullptr)
               == SQLITE_OK);
        sqlite3_bind_int64(stmt, 1, track1Id);
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        assert(sqlite3_column_int(stmt, 0) == 7);
        assert(sqlite3_column_int(stmt, 1) == 1);
        sqlite3_finalize(stmt);
        sqlite3_close(handle);
        std::ifstream in(exported, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(bytes.find(coverMarker) == std::string::npos);
    }
    std::cout << "case 9 (a cover kept in the database is not exported) OK\n";

    // The export audits clean: a row whose image was taken out keeps no
    // hash that names one, so it reads as Engine's empty "no cover" row
    // rather than a cover the export lost.
    {
        const ArtworkAudit audit = auditArtwork(seabass::pathToUtf8(destRoot));
        assert(audit.error.empty());
        assert(audit.unreadable.empty());
    }
    {
        sqlite3 *handle = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(destRoot / "Database2" / "m.db").c_str(), &handle) == SQLITE_OK);
        sqlite3_stmt *stmt = nullptr;
        assert(sqlite3_prepare_v2(handle, "SELECT typeof(hash), length(hash) FROM AlbumArt WHERE id = 8;", -1, &stmt,
                                  nullptr)
               == SQLITE_OK);
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        assert(std::string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0))) == "blob");
        assert(sqlite3_column_int(stmt, 1) == 20);
        sqlite3_finalize(stmt);
        sqlite3_close(handle);
    }
    std::cout << "case 10 (an export with its covers taken out audits clean) OK\n";

    // A schema whose AlbumArt has no albumArt column keeps no covers in the
    // database, and exports as any other.
    {
        const fs::path plainSource = root / "plain-source" / "Engine Library";
        const fs::path plainDest = root / "plain-dest" / "Engine Library";
        fs::create_directories(plainSource.parent_path());
        {
            auto db = djinterop::engine::create_database(seabass::pathToUtf8(plainSource));
            djinterop::track_snapshot snapshot;
            snapshot.title = "Plain";
            snapshot.relative_path = "../Contents/plain.mp3";
            db.create_track(snapshot);
        }
        sqlite3 *handle = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(plainSource / "Database2" / "m.db").c_str(), &handle) == SQLITE_OK);
        assert(sqlite3_exec(handle,
                            "PRAGMA foreign_keys = OFF; DROP INDEX IF EXISTS index_AlbumArt_hash; DROP TABLE AlbumArt; "
                            "CREATE TABLE AlbumArt (id INTEGER PRIMARY KEY AUTOINCREMENT, hash TEXT); "
                            "INSERT INTO AlbumArt (id, hash) VALUES (1, '');",
                            nullptr, nullptr, nullptr)
               == SQLITE_OK);
        sqlite3_close(handle);
        const auto plain = anonymizeEngineLibrary(seabass::pathToUtf8(plainSource), seabass::pathToUtf8(plainDest));
        if (!plain.errorMessage.empty()) {
            std::cerr << plain.errorMessage << "\n";
        }
        assert(plain.errorMessage.empty());
        assert(plain.albumArtImagesRemoved == 0);
    }
    std::cout << "case 11 (a schema without an image column exports) OK\n";

    // The covers leave the file even before it is compacted: the pages they
    // sat on are overwritten as they are cleared. And a compaction that
    // cannot run (another connection reading) says so.
    {
        const fs::path strip = root / "strip-only" / "Engine Library";
        fs::create_directories(strip / "Database2");
        const fs::path db = strip / "Database2" / "m.db";
        const std::string marker = "A-COVER-THAT-MUST-NOT-STAY-IN-THE-FILE";
        sqlite3 *handle = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(db).c_str(), &handle) == SQLITE_OK);
        assert(sqlite3_exec(handle, "CREATE TABLE AlbumArt (id INTEGER PRIMARY KEY, hash TEXT, albumArt BLOB);", nullptr,
                            nullptr, nullptr)
               == SQLITE_OK);
        sqlite3_stmt *insert = nullptr;
        assert(sqlite3_prepare_v2(handle, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (1, 'abc', ?);", -1, &insert,
                                  nullptr)
               == SQLITE_OK);
        const std::string image = std::string("\xFF\xD8\xFF", 3) + marker + std::string(3000, 'z');
        sqlite3_bind_blob(insert, 1, image.data(), static_cast<int>(image.size()), SQLITE_TRANSIENT);
        assert(sqlite3_step(insert) == SQLITE_DONE);
        sqlite3_finalize(insert);
        sqlite3_close(handle);
        assert(stripAlbumArtImages(seabass::pathToUtf8(strip)) == 1);
        std::ifstream in(db, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(bytes.find(marker) == std::string::npos);

        sqlite3 *reader = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(db).c_str(), &reader) == SQLITE_OK);
        assert(sqlite3_exec(reader, "BEGIN; SELECT count(*) FROM AlbumArt;", nullptr, nullptr, nullptr) == SQLITE_OK);
        assert(!compactDatabase(seabass::pathToUtf8(db)));
        sqlite3_exec(reader, "COMMIT;", nullptr, nullptr, nullptr);
        sqlite3_close(reader);
        assert(compactDatabase(seabass::pathToUtf8(db)));
    }
    std::cout << "case 12 (covers leave the file as they are cleared, and a failed compaction is said) OK\n";

    // A cover linked by path rather than kept: Engine's rekordbox import
    // writes "image://fileart//<path>" into AlbumArt.hash, a player as
    // /media/<stick label>/PIONEER/Artwork/..., a computer as wherever it
    // had the stick or the music. The label names the stick and a
    // computer's path names its user and folders, so neither may leave in
    // an export; the part under PIONEER/ is rekordbox's own numbering and
    // stays, so a link still reads as a link to an artwork file.
    {
        const fs::path linkSource = root / "link-source" / "Engine Library";
        const fs::path linkDest = root / "link-dest" / "Engine Library";
        fs::create_directories(linkSource.parent_path());
        {
            auto db = djinterop::engine::create_database(seabass::pathToUtf8(linkSource));
            djinterop::track_snapshot snapshot;
            snapshot.title = "Linked";
            snapshot.relative_path = "../Contents/linked.mp3";
            db.create_track(snapshot);
        }
        sqlite3 *handle = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(linkSource / "Database2" / "m.db").c_str(), &handle) == SQLITE_OK);
        const std::string onPlayer = "image://fileart//media/REALSTICKLABEL/PIONEER/Artwork/00002/a27_m.jpg";
        const std::string onComputer = "image://fileart//home/realusername/Music/Real Artist Folder/cover.jpg";
        sqlite3_stmt *insert = nullptr;
        assert(sqlite3_prepare_v2(handle, "INSERT INTO AlbumArt (id, hash, albumArt) VALUES (?, ?, NULL);", -1, &insert,
                                  nullptr)
               == SQLITE_OK);
        int id = 20;
        for (const std::string *link : {&onPlayer, &onComputer}) {
            sqlite3_bind_int(insert, 1, id++);
            sqlite3_bind_blob(insert, 2, link->data(), static_cast<int>(link->size()), SQLITE_TRANSIENT);
            assert(sqlite3_step(insert) == SQLITE_DONE);
            sqlite3_reset(insert);
        }
        sqlite3_finalize(insert);
        sqlite3_close(handle);

        const auto linked = anonymizeEngineLibrary(seabass::pathToUtf8(linkSource), seabass::pathToUtf8(linkDest));
        assert(linked.errorMessage.empty());
        assert(linked.artworkLinksScrubbed == 2);

        const fs::path exported = linkDest / "Database2" / "m.db";
        assert(sqlite3_open(seabass::pathToUtf8(exported).c_str(), &handle) == SQLITE_OK);
        sqlite3_stmt *stmt = nullptr;
        assert(sqlite3_prepare_v2(handle, "SELECT typeof(hash), CAST(hash AS TEXT) FROM AlbumArt WHERE id = ?;", -1,
                                  &stmt, nullptr)
               == SQLITE_OK);
        const auto hashOf = [&](int rowId) {
            sqlite3_bind_int(stmt, 1, rowId);
            assert(sqlite3_step(stmt) == SQLITE_ROW);
            assert(std::string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0))) == "blob");
            const std::string hash(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1)));
            sqlite3_reset(stmt);
            return hash;
        };
        assert(hashOf(20) == "image://fileart//media/STICK/PIONEER/Artwork/00002/a27_m.jpg");
        const std::string computer = hashOf(21);
        assert(computer.rfind("image://fileart//Contents/", 0) == 0);
        assert(computer.size() > std::string("image://fileart//Contents/").size());
        sqlite3_finalize(stmt);
        sqlite3_close(handle);

        std::ifstream in(exported, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (const char *gone : {"REALSTICKLABEL", "realusername", "Real Artist Folder"}) {
            assert(bytes.find(gone) == std::string::npos);
        }
    }
    std::cout << "case 13 (artwork links lose the stick label and the computer's path) OK\n";

    // A streaming track's link names the recording on the service as
    // surely as its title does: "streaming://TIDAL/Track/<the service's
    // id>". The service and the shape stay, so it still reads as a
    // streaming track; the id does not.
    {
        const fs::path streamSource = root / "stream-source" / "Engine Library";
        const fs::path streamDest = root / "stream-dest" / "Engine Library";
        fs::create_directories(streamSource.parent_path());
        int64_t streamId = 0;
        {
            auto db = djinterop::engine::create_database(seabass::pathToUtf8(streamSource));
            djinterop::track_snapshot snapshot;
            snapshot.title = "Streamed";
            snapshot.relative_path = "../Contents/streamed.mp4";
            streamId = db.create_track(snapshot).id();
        }
        sqlite3 *handle = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(streamSource / "Database2" / "m.db").c_str(), &handle) == SQLITE_OK);
        assert(sqlite3_exec(handle,
                            ("UPDATE Track SET streamingSource = 'TIDAL', uri = 'streaming://TIDAL/Track/918273645' "
                             "WHERE id = " + std::to_string(streamId) + ";").c_str(),
                            nullptr, nullptr, nullptr)
               == SQLITE_OK);
        sqlite3_close(handle);

        const auto streamed = anonymizeEngineLibrary(seabass::pathToUtf8(streamSource), seabass::pathToUtf8(streamDest));
        assert(streamed.errorMessage.empty());
        assert(streamed.streamingLinksScrubbed == 1);

        const fs::path exported = streamDest / "Database2" / "m.db";
        assert(sqlite3_open(seabass::pathToUtf8(exported).c_str(), &handle) == SQLITE_OK);
        sqlite3_stmt *stmt = nullptr;
        assert(sqlite3_prepare_v2(handle, "SELECT uri, streamingSource FROM Track WHERE id = ?;", -1, &stmt, nullptr)
               == SQLITE_OK);
        sqlite3_bind_int64(stmt, 1, streamId);
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        assert(std::string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)))
               == "streaming://TIDAL/Track/" + std::to_string(streamId));
        assert(std::string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1))) == "TIDAL");
        sqlite3_finalize(stmt);
        sqlite3_close(handle);
        std::ifstream in(exported, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(bytes.find("918273645") == std::string::npos);
    }
    std::cout << "case 14 (a streaming link loses the service's id for the recording) OK\n";

    // keepWaveforms wins over slimForTesting for the waveforms: the
    // overview blob and the .rgb previews stay, where slim alone drops
    // both. The titles go either way.
    {
        const fs::path waveSource = root / "wave-source" / "Engine Library";
        fs::create_directories(waveSource.parent_path());
        int64_t waveId = 0;
        {
            auto db = djinterop::engine::create_database(seabass::pathToUtf8(waveSource));
            djinterop::track_snapshot snapshot;
            snapshot.title = "Real Waveform Title";
            snapshot.relative_path = "../Contents/wave.mp3";
            snapshot.sample_count = 44100 * 10;
            snapshot.sample_rate = 44100;
            snapshot.waveform = std::vector<djinterop::waveform_entry>(
                1024, djinterop::waveform_entry{{10, 255}, {20, 255}, {30, 255}});
            waveId = db.create_track(snapshot).id();
        }
        const fs::path rgb = waveSource / "Database2" / "OverviewData" / "some-uuid" / (std::to_string(waveId) + ".rgb");
        fs::create_directories(rgb.parent_path());
        std::ofstream(rgb, std::ios::binary) << std::string(64, '\x7f');

        const auto overviewOf = [](const fs::path &engineLibrary) {
            sqlite3 *handle = nullptr;
            assert(sqlite3_open(seabass::pathToUtf8(engineLibrary / "Database2" / "m.db").c_str(), &handle) == SQLITE_OK);
            sqlite3_stmt *stmt = nullptr;
            assert(sqlite3_prepare_v2(handle, "SELECT overviewWaveFormData FROM PerformanceData;", -1, &stmt, nullptr)
                   == SQLITE_OK);
            assert(sqlite3_step(stmt) == SQLITE_ROW);
            const auto *bytes = static_cast<const char *>(sqlite3_column_blob(stmt, 0));
            std::string blob(bytes ? bytes : "", static_cast<std::size_t>(sqlite3_column_bytes(stmt, 0)));
            sqlite3_finalize(stmt);
            sqlite3_close(handle);
            return blob;
        };
        const std::string sourceOverview = overviewOf(waveSource);
        assert(!sourceOverview.empty());  // fixture self-check

        const fs::path slimDest = root / "wave-slim" / "Engine Library";
        const auto slim = anonymizeEngineLibrary(seabass::pathToUtf8(waveSource), seabass::pathToUtf8(slimDest),
                                                 /*slimForTesting=*/true, /*keepWaveforms=*/false);
        assert(slim.errorMessage.empty());
        assert(slim.waveformRowsEmptied == 1);
        assert(overviewOf(slimDest).empty());
        assert(!fs::exists(slimDest / "Database2" / "OverviewData"));

        const fs::path keptDest = root / "wave-kept" / "Engine Library";
        const auto kept = anonymizeEngineLibrary(seabass::pathToUtf8(waveSource), seabass::pathToUtf8(keptDest),
                                                 /*slimForTesting=*/true, /*keepWaveforms=*/true);
        assert(kept.errorMessage.empty());
        assert(kept.waveformRowsEmptied == 0);
        assert(overviewOf(keptDest) == sourceOverview);
        const fs::path keptRgb = keptDest / "Database2" / "OverviewData" / "some-uuid" / (std::to_string(waveId) + ".rgb");
        assert(fs::exists(keptRgb) && fs::file_size(keptRgb) == 64);
        auto keptDb = djinterop::engine::load_database(seabass::pathToUtf8(keptDest));
        assert(keptDb.track_by_id(waveId)->title() != std::optional<std::string>("Real Waveform Title"));
    }
    std::cout << "case 15 (keepWaveforms keeps the overview and the .rgb previews that slim drops) OK\n";

    std::cout << "all cases passed\n";
    return 0;
}
