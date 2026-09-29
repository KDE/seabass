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
    assert(seabass::pathFromUtf8(first.at("1")).parent_path() == cache);
    assert(first.at("3").empty());  // bytes no player can draw are not offered as a cover
    std::cout << "case 1 (covers kept in the database are shown) OK\n";

    // 2. A second read finds the same files and writes nothing new.
    const auto before = fs::last_write_time(seabass::pathFromUtf8(first.at("1")));
    const auto second = artworkBySourceId(seabass::pathToUtf8(library));
    assert(second.at("1") == first.at("1"));
    assert(second.at("2") == first.at("2"));
    assert(fs::last_write_time(seabass::pathFromUtf8(second.at("1"))) == before);
    std::cout << "case 2 (a later read reuses what the first wrote out) OK\n";

    fs::remove_all(library.parent_path(), ec);
    std::cout << "engine_reader_db_artwork_test passed\n";
    return 0;
}
