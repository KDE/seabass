// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Covers an older Engine library keeps inside its database.
//
// Such a library stores each image in AlbumArt.albumArt beside a hash
// written as hex text, and has no Artwork/ files at all. The reader showed
// none of them. Run over a copy of the fixture's Engine library with a few
// rows turned into that storage, one of them at id 1.

#include <sqlite3.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "application/ports/progress_reporter.hpp"
#include "infrastructure/engine/engine_artwork.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;

namespace
{

struct QuietReporter : seabass::application::ProgressReporter
{
    void start(const std::string &, size_t) override {}
    void tick(size_t) override {}
    void finish() override {}
    void warn(const std::string &) override {}
};

void setRow(sqlite3 *db, int id, const std::string &hash, const std::string &image)
{
    sqlite3_stmt *stmt = nullptr;
    assert(sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO AlbumArt (id, hash, albumArt) VALUES (?, ?, ?);", -1, &stmt,
                              nullptr)
           == SQLITE_OK);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 3, image.data(), static_cast<int>(image.size()), SQLITE_TRANSIENT);
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
}

std::string slurp(const std::string &file)
{
    std::ifstream in(seabass::pathFromUtf8(file), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::map<std::string, std::string> artworkBySourceId(const std::string &library)
{
    QuietReporter reporter;
    seabass::infrastructure::engine::LibdjinteropEngineReader reader(library);
    reader.setProgressReporter(reporter);
    std::map<std::string, std::string> result;
    for (const auto &track : reader.readAll()) {
        result[track.sourceId] = track.artworkPath;
    }
    return result;
}

}  // namespace

int main()
{
    const fs::path source =
        seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "engine";
    const fs::path library = seabass::testing::scratchRoot() / "seabass_engine_reader_db_artwork" / "Engine Library";
    std::error_code ec;
    fs::remove_all(library.parent_path(), ec);
    fs::create_directories(library.parent_path());
    fs::copy(source, library, fs::copy_options::recursive);
    fs::remove_all(seabass::infrastructure::paths::localEngineArtworkDir(), ec);

    const std::string pngImage = std::string("\x89PNG\r\n\x1a\n", 8) + "A-PNG-KEPT-IN-THE-ROW";
    const std::string jpegImage = std::string("\xFF\xD8\xFF", 3) + "A-JPEG-KEPT-IN-THE-ROW";
    {
        sqlite3 *db = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(library / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
        // Track n points at AlbumArt n in this fixture.
        setRow(db, 1, "af2f6f87c56583adb67003735089017e2eb03572", pngImage);
        setRow(db, 2, "551c96558e2eb05ea31f3735b129f242b720c15", jpegImage);  // 39 characters
        setRow(db, 3, "8998055a7787a03a8e8de2fa607a11f37b4c674a", "NOT A PICTURE");
        sqlite3_close(db);
    }

    // 1. The covers show, as files holding exactly the row's image, kept
    //    on this computer rather than written onto the stick.
    const auto first = artworkBySourceId(seabass::pathToUtf8(library));
    assert(first.count("1") && first.count("2") && first.count("3"));
    assert(!first.at("1").empty());
    assert(!first.at("2").empty());
    assert(slurp(first.at("1")) == pngImage);
    assert(slurp(first.at("2")) == jpegImage);
    assert(seabass::pathFromUtf8(first.at("1")).extension() == ".png");
    assert(seabass::pathFromUtf8(first.at("2")).extension() == ".jpg");
    const fs::path cache = seabass::infrastructure::paths::localEngineArtworkDir();
    assert(seabass::pathFromUtf8(first.at("1")).parent_path().parent_path() == cache);  // one directory per library
    assert(first.at("3").empty());  // bytes no player can draw are not offered as a cover
    std::cout << "case 1 (covers kept in the database are shown) OK\n";

    // 2. A second read finds the same files and writes nothing new.
    const auto before = fs::last_write_time(seabass::pathFromUtf8(first.at("1")));
    const auto second = artworkBySourceId(seabass::pathToUtf8(library));
    assert(second.at("1") == first.at("1"));
    assert(second.at("2") == first.at("2"));
    assert(fs::last_write_time(seabass::pathFromUtf8(second.at("1"))) == before);
    std::cout << "case 2 (a later read reuses what the first wrote out) OK\n";

    // 3. Writing the images out is not the track list's to wait for: the
    //    first stage of a progressive read names no such cover and writes
    //    nothing, and the artwork stage brings them in.
    {
        fs::remove_all(cache, ec);
        QuietReporter reporter;
        seabass::infrastructure::engine::LibdjinteropEngineReader reader(seabass::pathToUtf8(library));
        reader.setProgressReporter(reporter);
        auto tracks = reader.readTracks();
        for (const auto &track : tracks) {
            if (track.sourceId == "1" || track.sourceId == "2") {
                assert(track.artworkPath.empty());
            }
        }
        assert(!fs::exists(cache) || fs::is_empty(cache));
        reader.fillArtwork(tracks);
        for (const auto &track : tracks) {
            if (track.sourceId == "1") {
                assert(slurp(track.artworkPath) == pngImage);
            }
        }
        std::cout << "case 3 (the track list does not wait for covers kept in the database) OK\n";
    }

    // 4. Two libraries whose rows carry the same hash over different
    //    images: each shows its own. The hash is Engine's, not a checksum
    //    of the bytes, so it names an image only inside one library.
    {
        const fs::path other = library.parent_path() / "other" / "Engine Library";
        fs::create_directories(other.parent_path());
        fs::copy(source, other, fs::copy_options::recursive);
        const std::string otherImage = std::string("\x89PNG\r\n\x1a\n", 8) + "ANOTHER-LIBRARY-S-PNG";
        sqlite3 *db = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(other / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
        setRow(db, 1, "af2f6f87c56583adb67003735089017e2eb03572", otherImage);
        assert(sqlite3_exec(db, "UPDATE Information SET uuid = '11111111-2222-3333-4444-555555555555';", nullptr,
                            nullptr, nullptr)
               == SQLITE_OK);
        sqlite3_close(db);
        const auto mine = artworkBySourceId(seabass::pathToUtf8(library));
        const auto theirs = artworkBySourceId(seabass::pathToUtf8(other));
        assert(slurp(mine.at("1")) == pngImage);
        assert(slurp(theirs.at("1")) == otherImage);
        // And each still costs a stat the next time.
        const auto again = artworkBySourceId(seabass::pathToUtf8(other));
        assert(again.at("1") == theirs.at("1"));
        std::cout << "case 4 (two libraries with the same hash keep their own covers) OK\n";
    }

    // 5. Current libraries keep each cover as a file under Artwork/, named
    //    by the row's 20-byte hash. The track names that file itself, on
    //    the stick; one whose file is gone names none.
    {
        const std::vector<std::uint8_t> present(20, 0x5A);
        const std::vector<std::uint8_t> gone(20, 0x6B);
        const std::string presentName = seabass::infrastructure::engine::artworkFileName(present);
        const fs::path artwork = library / "Artwork" / (presentName + ".png");
        fs::create_directories(artwork.parent_path());
        std::ofstream(artwork, std::ios::binary) << pngImage;
        sqlite3 *db = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(library / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
        for (const auto &[id, hash] : {std::pair{4, &present}, std::pair{5, &gone}}) {
            sqlite3_stmt *stmt = nullptr;
            assert(sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO AlbumArt (id, hash, albumArt) VALUES (?, ?, NULL);", -1,
                                      &stmt, nullptr)
                   == SQLITE_OK);
            sqlite3_bind_int(stmt, 1, id);
            sqlite3_bind_blob(stmt, 2, hash->data(), static_cast<int>(hash->size()), SQLITE_TRANSIENT);
            assert(sqlite3_step(stmt) == SQLITE_DONE);
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
        const auto read = artworkBySourceId(seabass::pathToUtf8(library));
        assert(!read.at("4").empty());
        assert(fs::equivalent(seabass::pathFromUtf8(read.at("4")), artwork));
        assert(read.at("5").empty());
        std::cout << "case 5 (a cover kept as a file under Artwork/ is shown from the stick) OK\n";
    }

    // 6. Bytes in albumArt that are no image do not hide the art the
    //    row's hash names: an imported rekordbox path, or a file under
    //    Artwork/, is shown as if the bytes were not there.
    {
        const fs::path imported = library.parent_path() / "PIONEER" / "Artwork" / "00001" / "a6.jpg";
        fs::create_directories(imported.parent_path());
        std::ofstream(imported, std::ios::binary) << jpegImage;
        const std::vector<std::uint8_t> fileHash(20, 0x3C);
        const fs::path onStick =
            library / "Artwork" / (seabass::infrastructure::engine::artworkFileName(fileHash) + ".jpg");
        std::ofstream(onStick, std::ios::binary) << jpegImage;
        sqlite3 *db = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(library / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
        setRow(db, 6, "image://fileart//media/ELSEWHERE/PIONEER/Artwork/00001/a6.jpg", "JUNK");
        sqlite3_stmt *stmt = nullptr;
        assert(sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO AlbumArt (id, hash, albumArt) VALUES (7, ?, 'JUNK');", -1,
                                  &stmt, nullptr)
               == SQLITE_OK);
        sqlite3_bind_blob(stmt, 1, fileHash.data(), static_cast<int>(fileHash.size()), SQLITE_TRANSIENT);
        assert(sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        const auto read = artworkBySourceId(seabass::pathToUtf8(library));
        assert(!read.at("6").empty() && fs::equivalent(seabass::pathFromUtf8(read.at("6")), imported));
        assert(!read.at("7").empty() && fs::equivalent(seabass::pathFromUtf8(read.at("7")), onStick));
        std::cout << "case 6 (junk in albumArt does not hide the art the hash names) OK\n";
    }

    fs::remove_all(library.parent_path(), ec);
    std::cout << "engine_reader_db_artwork_test passed\n";
    return 0;
}
