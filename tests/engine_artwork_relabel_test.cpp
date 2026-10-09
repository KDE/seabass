// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A Denon player's rekordbox import writes each cover as a link to the
// stick as the player mounts it, "image://fileart//media/<label>/...".
// Copied onto a stick with another label, those links name a mount that
// stick never gets: WS_NEW, made from WHALESHARK's backup, kept 1361 rows
// saying /media/WHALESHARK/ and a player showed 186 of its covers.
// relabelImportedArtworkLinks() renames exactly those links and nothing
// else, byte for byte.

#include <cassert>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <tuple>

#include <sqlite3.h>

#include "infrastructure/engine/engine_artwork.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass::infrastructure::engine;
using seabass::pathFromUtf8;
using seabass::pathToUtf8;

namespace
{

void exec(sqlite3 *db, const std::string &sql)
{
    char *error = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        std::cerr << "sql failed: " << (error ? error : "?") << " for " << sql << "\n";
        assert(false);
    }
}

// One AlbumArt row as stored: the hash's storage class and bytes, and the
// image's.
struct Row
{
    int hashType = SQLITE_NULL;
    std::string hash;
    int imageType = SQLITE_NULL;
    std::string image;
    bool operator==(const Row &other) const
    {
        return std::tie(hashType, hash, imageType, image)
               == std::tie(other.hashType, other.hash, other.imageType, other.image);
    }
};

std::string bytesOf(sqlite3_stmt *stmt, int column)
{
    const void *data = sqlite3_column_blob(stmt, column);
    const int size = sqlite3_column_bytes(stmt, column);
    return data == nullptr ? std::string() : std::string(static_cast<const char *>(data), static_cast<size_t>(size));
}

std::map<std::int64_t, Row> albumArt(const fs::path &file)
{
    sqlite3 *db = nullptr;
    assert(sqlite3_open_v2(pathToUtf8(file).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt *stmt = nullptr;
    assert(sqlite3_prepare_v2(db, "SELECT id, hash, albumArt FROM AlbumArt;", -1, &stmt, nullptr) == SQLITE_OK);
    std::map<std::int64_t, Row> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Row row;
        row.hashType = sqlite3_column_type(stmt, 1);
        row.hash = bytesOf(stmt, 1);
        row.imageType = sqlite3_column_type(stmt, 2);
        row.image = bytesOf(stmt, 2);
        rows[sqlite3_column_int64(stmt, 0)] = row;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rows;
}

// Every Track row, as text, to show the relabel touched no other table.
std::string tracks(const fs::path &file)
{
    sqlite3 *db = nullptr;
    assert(sqlite3_open_v2(pathToUtf8(file).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt *stmt = nullptr;
    assert(sqlite3_prepare_v2(db, "SELECT * FROM Track ORDER BY id;", -1, &stmt, nullptr) == SQLITE_OK);
    std::string out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        for (int c = 0; c < sqlite3_column_count(stmt); ++c) {
            out += std::to_string(sqlite3_column_type(stmt, c)) + ":" + bytesOf(stmt, c) + "|";
        }
        out += "\n";
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return out;
}

// Inserts one row with the hash bound as a blob (how Engine's import
// stores its links) or as text, and returns its id.
std::int64_t plant(sqlite3 *db, const std::string &hash, bool hashAsBlob, const std::string &image = {})
{
    sqlite3_stmt *stmt = nullptr;
    assert(sqlite3_prepare_v2(db, "INSERT INTO AlbumArt (hash, albumArt) VALUES (?, ?);", -1, &stmt, nullptr)
           == SQLITE_OK);
    if (hashAsBlob) {
        sqlite3_bind_blob(stmt, 1, hash.data(), static_cast<int>(hash.size()), SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_text(stmt, 1, hash.data(), static_cast<int>(hash.size()), SQLITE_TRANSIENT);
    }
    if (image.empty()) {
        sqlite3_bind_null(stmt, 2);
    } else {
        sqlite3_bind_blob(stmt, 2, image.data(), static_cast<int>(image.size()), SQLITE_TRANSIENT);
    }
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return sqlite3_last_insert_rowid(db);
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_engine_artwork_relabel_test";
    fs::remove_all(root);
    const fs::path library = root / "STICKA" / "Engine Library";
    fs::create_directories(library);
    // The committed real-scale library, whose own 1469 links all name
    // WHALESHARK2: a third label, there in bulk, that no relabel of
    // STICKA may touch.
    fs::copy(pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "engine", library,
             fs::copy_options::recursive);
    const fs::path db = library / "Database2" / "m.db";

    const std::string tail = "/PIONEER/Artwork/00001/a5_m.jpg";
    std::int64_t linkA1 = 0, linkA2 = 0, linkA3 = 0, linkB = 0, linkPrefix = 0, inDatabase = 0, fileBacked = 0;
    {
        sqlite3 *handle = nullptr;
        assert(sqlite3_open(pathToUtf8(db).c_str(), &handle) == SQLITE_OK);
        // Three links under A, in the spellings a player and a FAT label
        // can give it: the label compares without case.
        linkA1 = plant(handle, "image://fileart//media/STICKA" + tail, true);
        linkA2 = plant(handle, "image://fileart//media/sticka/PIONEER/Artwork/00002/b7_m.jpg", true);
        linkA3 = plant(handle, "image://fileart//media/StickA/PIONEER/Artwork/00003/\xC3\xA9t\xC3\xA9.jpg", false);
        // One under another label, and one whose label only starts with A.
        linkB = plant(handle, "image://fileart//media/OTHERB" + tail, true);
        linkPrefix = plant(handle, "image://fileart//media/STICKAB" + tail, true);
        // An image kept in the row, under a hex text hash, and a row
        // naming a file by its 20-byte hash.
        inDatabase = plant(handle, "af2f6f87c56583adb67003735089017e2eb03572", false,
                           std::string("\xFF\xD8\xFF", 3) + "image://fileart//media/STICKA" + tail);
        fileBacked = plant(handle, std::string("image://fileart//media/STICKA/\x01\x02\x03\x04\x05", 34), true);
        exec(handle, "UPDATE AlbumArt SET hash = randomblob(20) WHERE id = " + std::to_string(fileBacked) + ";");
        sqlite3_close(handle);
    }
    const std::map<std::int64_t, Row> before = albumArt(db);
    const std::string tracksBefore = tracks(db);
    assert(before.size() > 1500 && "the fixture's own rows are there too");

    // 1. The count the page would show, read-only.
    {
        std::string error;
        assert(countImportedArtworkLinks(pathToUtf8(db), "STICKA", &error) == 3);
        assert(error.empty());
        assert(countImportedArtworkLinks(pathToUtf8(db), "sticka", &error) == 3 && "the label compares without case");
        assert(countImportedArtworkLinks(pathToUtf8(db), "OTHERB", &error) == 1);
        assert(countImportedArtworkLinks(pathToUtf8(db), "WHALESHARK2", &error) == 1469);
        assert(countImportedArtworkLinks(pathToUtf8(db), "NOBODY", &error) == 0);
        assert(albumArt(db) == before && "counting writes nothing");
        std::cout << "case 1 (count finds the links to one label, without case) OK\n";
    }

    // 2. A to C rewrites exactly the three, byte for byte, each in the
    //    storage class it had.
    {
        std::string error;
        const int renamed = relabelImportedArtworkLinks(pathToUtf8(db), "StickA", "C", &error);
        if (renamed != 3) {
            std::cerr << "renamed " << renamed << " rows: " << error << "\n";
        }
        assert(renamed == 3);
        assert(error.empty());
        const std::map<std::int64_t, Row> after = albumArt(db);
        assert(after.size() == before.size());
        Row expected1 = before.at(linkA1);
        expected1.hash = "image://fileart//media/C" + tail;
        Row expected2 = before.at(linkA2);
        expected2.hash = "image://fileart//media/C/PIONEER/Artwork/00002/b7_m.jpg";
        Row expected3 = before.at(linkA3);
        expected3.hash = "image://fileart//media/C/PIONEER/Artwork/00003/\xC3\xA9t\xC3\xA9.jpg";
        assert(after.at(linkA1) == expected1);
        assert(after.at(linkA2) == expected2);
        assert(after.at(linkA3) == expected3);
        assert(after.at(linkA1).hashType == SQLITE_BLOB && "a blob link stays a blob");
        assert(after.at(linkA3).hashType == SQLITE_TEXT && "a text link stays text");
        for (const auto &[id, row] : before) {
            if (id == linkA1 || id == linkA2 || id == linkA3) {
                continue;
            }
            assert(after.at(id) == row && "every other row is byte-identical");
        }
        assert(after.at(linkB) == before.at(linkB));
        assert(after.at(linkPrefix) == before.at(linkPrefix));
        assert(after.at(inDatabase) == before.at(inDatabase));
        assert(after.at(fileBacked) == before.at(fileBacked));
        assert(tracks(db) == tracksBefore && "no track is touched");
        assert(countImportedArtworkLinks(pathToUtf8(db), "STICKA", &error) == 0);
        assert(countImportedArtworkLinks(pathToUtf8(db), "C", &error) == 3);
        std::cout << "case 2 (A to C renames exactly the three links, byte-exact elsewhere) OK\n";
    }

    // 3. Run again, nothing is left to do; a relabel to the same name
    //    leaves the rows alone.
    {
        std::string error;
        const std::map<std::int64_t, Row> settled = albumArt(db);
        assert(relabelImportedArtworkLinks(pathToUtf8(db), "STICKA", "C", &error) == 0);
        assert(relabelImportedArtworkLinks(pathToUtf8(db), "C", "C", &error) == 0);
        assert(albumArt(db) == settled);
        std::cout << "case 3 (a second run and a same-name run change nothing) OK\n";
    }

    // 4. Errors: a database that is not there is reported and never
    //    created, and labels that cannot be a mount name are refused.
    {
        std::string error;
        const fs::path missing = root / "nowhere" / "m.db";
        assert(relabelImportedArtworkLinks(pathToUtf8(missing), "STICKA", "C", &error) == -1);
        assert(!error.empty());
        assert(!fs::exists(missing));
        error.clear();
        assert(countImportedArtworkLinks(pathToUtf8(missing), "STICKA", &error) == -1);
        assert(!error.empty());
        assert(!fs::exists(missing));
        const std::map<std::int64_t, Row> settled = albumArt(db);
        for (const auto &[from, to] : {std::pair<std::string, std::string>{"", "C"}, {"OTHERB", ""}, {"OTHERB", "A/B"}}) {
            error.clear();
            assert(relabelImportedArtworkLinks(pathToUtf8(db), from, to, &error) == -1);
            assert(!error.empty());
        }
        assert(albumArt(db) == settled && "a refused relabel writes nothing");
        std::cout << "case 4 (missing database and impossible labels are errors) OK\n";
    }

    std::cout << "All engine_artwork_relabel tests passed." << std::endl;
    return 0;
}
