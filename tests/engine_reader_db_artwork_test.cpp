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
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <thread>
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
    std::vector<std::string> warnings;
    void start(const std::string &, size_t) override {}
    void tick(size_t) override {}
    void finish() override {}
    void warn(const std::string &message) override { warnings.push_back(message); }
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
    assert(seabass::pathFromUtf8(first.at("1")).parent_path().parent_path().parent_path() == cache);  // per library and stick
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

    // 7. A clone (Backup USB Stick) keeps the library's uuid, so two
    //    libraries can share uuid and hash over different images. Each
    //    shows its own: a copy whose size is not the row's is written
    //    again.
    {
        const fs::path clone = library.parent_path() / "clone" / "Engine Library";
        fs::create_directories(clone.parent_path());
        fs::copy(source, clone, fs::copy_options::recursive);
        const std::string cloneImage = std::string("\x89PNG\r\n\x1a\n", 8) + "THE-CLONE-HAS-A-LONGER-PNG-HERE";
        sqlite3 *db = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(clone / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
        setRow(db, 1, "af2f6f87c56583adb67003735089017e2eb03572", cloneImage);
        sqlite3_close(db);
        const auto mine = artworkBySourceId(seabass::pathToUtf8(library));
        assert(slurp(mine.at("1")) == pngImage);
        const auto theirs = artworkBySourceId(seabass::pathToUtf8(clone));
        assert(slurp(theirs.at("1")) == cloneImage);
        std::cout << "case 7 (a clone with the same uuid and hash shows its own cover) OK\n";
    }

    // 8. A stop lands within one row of the artwork stage, before any
    //    image is written out.
    {
        fs::remove_all(cache, ec);
        QuietReporter reporter;
        seabass::application::CancellationToken cancel;
        seabass::infrastructure::engine::LibdjinteropEngineReader reader(seabass::pathToUtf8(library));
        reader.setProgressReporter(reporter);
        reader.setCancellationToken(cancel);
        auto tracks = reader.readTracks();
        cancel.cancel();
        bool stopped = false;
        try {
            reader.fillArtwork(tracks);
        } catch (const seabass::application::OperationCancelled &) {
            stopped = true;
        }
        assert(stopped);
        assert(!fs::exists(cache) || fs::is_empty(cache));
        std::cout << "case 8 (a stop lands before the artwork stage writes anything) OK\n";
    }

    // 9. An artwork stage that cannot read the database says so: covers
    //    missing because of a failed read must not look like a library
    //    that has none.
    {
        const fs::path broken = library.parent_path() / "broken" / "Engine Library";
        fs::create_directories(broken.parent_path());
        fs::copy(source, broken, fs::copy_options::recursive);
        sqlite3 *db = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(broken / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
        assert(sqlite3_exec(db, "ALTER TABLE AlbumArt RENAME TO Gone;", nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
        QuietReporter reporter;
        seabass::infrastructure::engine::LibdjinteropEngineReader reader(seabass::pathToUtf8(broken));
        reader.setProgressReporter(reporter);
        std::vector<seabass::domain::Track> none;
        reader.fillArtwork(none);
        assert(reporter.warnings.size() == 1);
        assert(reporter.warnings[0].find("album art") != std::string::npos);
        std::cout << "case 9 (an artwork stage that cannot read the database warns) OK\n";
    }

    // 10. Two reads of the same library at once (the prefetch and a page)
    //     write out the same covers side by side. Each still names a whole
    //     copy of every one.
    {
        const fs::path busy = library.parent_path() / "busy" / "Engine Library";
        fs::create_directories(busy.parent_path());
        fs::copy(source, busy, fs::copy_options::recursive);
        sqlite3 *db = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(busy / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
        assert(sqlite3_exec(db, "BEGIN; DELETE FROM AlbumArt;", nullptr, nullptr, nullptr) == SQLITE_OK);
        for (int row = 1; row <= 300; ++row) {
            char hash[41];
            std::snprintf(hash, sizeof(hash), "%040x", row);
            setRow(db, row, hash, pngImage + std::string(static_cast<size_t>(4000 + row), 'p'));
        }
        assert(sqlite3_exec(db, "UPDATE Track SET albumArtId = ((id - 1) % 300) + 1; COMMIT;", nullptr, nullptr,
                            nullptr)
               == SQLITE_OK);
        sqlite3_close(db);
        for (int round = 0; round < 5; ++round) {
            fs::remove_all(cache, ec);
            std::map<std::string, std::string> results[2];
            std::thread first([&] { results[0] = artworkBySourceId(seabass::pathToUtf8(busy)); });
            std::thread second([&] { results[1] = artworkBySourceId(seabass::pathToUtf8(busy)); });
            first.join();
            second.join();
            for (const auto &result : results) {
                assert(result.size() > 300);
                for (const auto &[sourceId, path] : result) {
                    const int row = (std::stoi(sourceId) - 1) % 300 + 1;
                    assert(!path.empty());
                    assert(slurp(path) == pngImage + std::string(static_cast<size_t>(4000 + row), 'p'));
                }
            }
        }
        std::cout << "case 10 (two reads writing the same covers at once both get whole copies) OK\n";
    }

    // 11. A schema whose AlbumArt has no albumArt column keeps no images
    //     in the database, and its files under Artwork/ still show,
    //     without a warning.
    {
        const fs::path older = library.parent_path() / "no-image-column" / "Engine Library";
        fs::create_directories(older.parent_path());
        fs::copy(source, older, fs::copy_options::recursive);
        const std::vector<std::uint8_t> fileHash(20, 0x4D);
        const fs::path onStick =
            older / "Artwork" / (seabass::infrastructure::engine::artworkFileName(fileHash) + ".jpg");
        fs::create_directories(onStick.parent_path());
        std::ofstream(onStick, std::ios::binary) << jpegImage;
        sqlite3 *db = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(older / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
        assert(sqlite3_exec(db,
                            "PRAGMA foreign_keys = OFF; DROP INDEX IF EXISTS index_AlbumArt_hash; DROP TABLE AlbumArt; "
                            "CREATE TABLE AlbumArt (id INTEGER PRIMARY KEY AUTOINCREMENT, hash BLOB);",
                            nullptr, nullptr, nullptr)
               == SQLITE_OK);
        sqlite3_stmt *stmt = nullptr;
        assert(sqlite3_prepare_v2(db, "INSERT INTO AlbumArt (id, hash) VALUES (2, ?);", -1, &stmt, nullptr) == SQLITE_OK);
        sqlite3_bind_blob(stmt, 1, fileHash.data(), static_cast<int>(fileHash.size()), SQLITE_TRANSIENT);
        assert(sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        QuietReporter reporter;
        seabass::infrastructure::engine::LibdjinteropEngineReader reader(seabass::pathToUtf8(older));
        reader.setProgressReporter(reporter);
        auto tracks = reader.readTracks();
        reporter.warnings.clear();
        reader.fillArtwork(tracks);
        for (const auto &warning : reporter.warnings) {
            std::cerr << "warning: " << warning << "\n";
        }
        assert(reporter.warnings.empty());
        bool shown = false;
        for (const auto &track : tracks) {
            if (track.sourceId == "2") {
                shown = !track.artworkPath.empty() && fs::equivalent(seabass::pathFromUtf8(track.artworkPath), onStick);
            }
        }
        assert(shown);
        std::cout << "case 11 (no albumArt column: covers under Artwork/ still show, no warning) OK\n";
    }

    // 12. A writer holding the database for a moment (a save committing)
    //     is waited out, not reported as covers that are not there.
    {
        fs::remove_all(cache, ec);
        QuietReporter reporter;
        seabass::infrastructure::engine::LibdjinteropEngineReader reader(seabass::pathToUtf8(library));
        reader.setProgressReporter(reporter);
        auto tracks = reader.readTracks();
        reporter.warnings.clear();
        sqlite3 *writer = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(library / "Database2" / "m.db").c_str(), &writer) == SQLITE_OK);
        assert(sqlite3_exec(writer, "BEGIN EXCLUSIVE;", nullptr, nullptr, nullptr) == SQLITE_OK);
        std::thread release([writer] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            sqlite3_exec(writer, "COMMIT;", nullptr, nullptr, nullptr);
        });
        reader.fillArtwork(tracks);
        release.join();
        sqlite3_close(writer);
        for (const auto &warning : reporter.warnings) {
            std::cerr << "warning: " << warning << "\n";
        }
        assert(reporter.warnings.empty());
        bool shown = false;
        for (const auto &track : tracks) {
            shown = shown || (track.sourceId == "1" && slurp(track.artworkPath) == pngImage);
        }
        assert(shown);
        std::cout << "case 12 (a writer committing is waited out) OK\n";
    }

    // 13. Clones that went their own ways can share uuid, hash and even the
    //     image's length over different bytes. The copies are kept per
    //     stick (its filesystem's identity, when the page knows it) or per
    //     library location, so each shows its own, and a later read still
    //     finds its copy with a stat.
    {
        const std::string first = std::string("\x89PNG\r\n\x1a\n", 8) + "SAME-LENGTH-IMAGE-AAAA";
        const std::string second = std::string("\x89PNG\r\n\x1a\n", 8) + "SAME-LENGTH-IMAGE-BBBB";
        std::vector<fs::path> clones;
        for (const char *name : {"clone-a", "clone-b"}) {
            const fs::path clone = library.parent_path() / name / "Engine Library";
            fs::create_directories(clone.parent_path());
            fs::copy(source, clone, fs::copy_options::recursive);
            sqlite3 *db = nullptr;
            assert(sqlite3_open(seabass::pathToUtf8(clone / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
            setRow(db, 1, "af2f6f87c56583adb67003735089017e2eb03572", clones.empty() ? first : second);
            sqlite3_close(db);
            clones.push_back(clone);
        }
        const auto a = artworkBySourceId(seabass::pathToUtf8(clones[0]));
        const auto b = artworkBySourceId(seabass::pathToUtf8(clones[1]));
        assert(slurp(a.at("1")) == first);
        assert(slurp(b.at("1")) == second);

        // With the stick's identity given, that is the key: the same stick
        // at another path finds the same copy.
        QuietReporter reporter;
        seabass::infrastructure::engine::LibdjinteropEngineReader reader(seabass::pathToUtf8(clones[0]));
        reader.setProgressReporter(reporter);
        reader.setVolumeIdentity("1234-ABCD");
        auto tracks = reader.readTracks();
        reader.fillArtwork(tracks);
        for (const auto &track : tracks) {
            if (track.sourceId == "1") {
                assert(track.artworkPath.find("1234-ABCD") != std::string::npos);
                assert(slurp(track.artworkPath) == first);
            }
        }
        std::cout << "case 13 (clones with the same uuid, hash and length keep their own covers) OK\n";
    }

    // 14. A row with a blob hash and an image kept beside it is named by
    //     that hash, so a later read finds its copy with a stat instead of
    //     reading and checksumming the image again.
    {
        const std::vector<std::uint8_t> blobHash(20, 0xAB);
        sqlite3 *db = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(library / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
        sqlite3_stmt *stmt = nullptr;
        assert(sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO AlbumArt (id, hash, albumArt) VALUES (8, ?, ?);", -1, &stmt,
                                  nullptr)
               == SQLITE_OK);
        sqlite3_bind_blob(stmt, 1, blobHash.data(), static_cast<int>(blobHash.size()), SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 2, pngImage.data(), static_cast<int>(pngImage.size()), SQLITE_TRANSIENT);
        assert(sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        const auto read = artworkBySourceId(seabass::pathToUtf8(library));
        const std::string expected = std::string("abababababababababababababababababababab") + "-"
            + std::to_string(pngImage.size()) + ".png";
        assert(seabass::pathToUtf8(seabass::pathFromUtf8(read.at("8")).filename()) == expected);
        assert(slurp(read.at("8")) == pngImage);
        std::cout << "case 14 (an image beside a blob hash is named by that hash) OK\n";
    }

    // 15. Tidying the local copies, once per library per run: image files
    //     straight under the library's folder (the layout before copies
    //     were kept per stick) go, and so does a location's folder nobody
    //     has written to for 30 days. The folder in use, and a recent one,
    //     stay.
    {
        const fs::path tidy = library.parent_path() / "tidy" / "Engine Library";
        fs::create_directories(tidy.parent_path());
        fs::copy(source, tidy, fs::copy_options::recursive);
        const std::string uuid = "33333333-4444-5555-6666-777777777777";
        sqlite3 *db = nullptr;
        assert(sqlite3_open(seabass::pathToUtf8(tidy / "Database2" / "m.db").c_str(), &db) == SQLITE_OK);
        setRow(db, 1, "af2f6f87c56583adb67003735089017e2eb03572", pngImage);
        assert(sqlite3_exec(db, ("UPDATE Information SET uuid = '" + uuid + "';").c_str(), nullptr, nullptr, nullptr)
               == SQLITE_OK);
        sqlite3_close(db);
        const fs::path libraryCopies = cache / uuid;
        const fs::path oldLayout = libraryCopies / "af2f6f87c56583adb67003735089017e2eb03572.png";
        const fs::path stale = libraryCopies / "at-0123456789abcdef" / "old-10.png";
        const fs::path recent = libraryCopies / "at-fedcba9876543210" / "new-10.png";
        for (const fs::path &file : {oldLayout, stale, recent}) {
            fs::create_directories(file.parent_path());
            std::ofstream(file, std::ios::binary) << "0123456789";
        }
        fs::last_write_time(stale, fs::file_time_type::clock::now() - std::chrono::hours(24 * 31));
        const auto read = artworkBySourceId(seabass::pathToUtf8(tidy));
        assert(slurp(read.at("1")) == pngImage);
        assert(!fs::exists(oldLayout));
        assert(!fs::exists(stale.parent_path()));
        assert(fs::exists(recent));
        assert(fs::exists(seabass::pathFromUtf8(read.at("1"))));
        std::cout << "case 15 (old layout and forgotten locations are tidied away) OK\n";
    }

    fs::remove_all(library.parent_path(), ec);
    std::cout << "engine_reader_db_artwork_test passed\n";
    return 0;
}
