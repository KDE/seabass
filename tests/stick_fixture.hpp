// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

// Helpers for building a fake DJ stick in a temp directory, shared by the
// backup / restore / clone tests: deterministic file contents, files with
// a chosen mtime, a minimal SQLite database where Engine's m.db lives,
// and a content snapshot of a tree as the stat walker sees it.

#include <sqlite3.h>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"

namespace seabass::test_fixture
{

namespace fs = std::filesystem;

inline std::string pseudoRandom(std::size_t size, std::uint64_t seed)
{
    std::string out(size, '\0');
    std::uint64_t x = seed * 0x9E3779B97F4A7C15ull + 1;
    for (std::size_t i = 0; i < size; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out[i] = static_cast<char>(x & 0xff);
    }
    return out;
}

inline void writeFile(const fs::path &p, const std::string &content, std::int64_t mtime)
{
    fs::create_directories(p.parent_path());
    {
        std::ofstream out(p, std::ios::binary);
        out << content;
    }
    fs::last_write_time(p, infrastructure::stick_backup::fromUnixSeconds(mtime));
}

inline std::string readFile(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline void createEngineDb(const fs::path &path)
{
    fs::create_directories(path.parent_path());
    sqlite3 *db = nullptr;
    assert(sqlite3_open(seabass::pathToUtf8(path).c_str(), &db) == SQLITE_OK);
    char *error = nullptr;
    assert(sqlite3_exec(db, "CREATE TABLE Track(id INTEGER PRIMARY KEY, path TEXT); INSERT INTO Track(path) VALUES('Contents/a.mp3')",
                        nullptr, nullptr, &error) == SQLITE_OK);
    sqlite3_close(db);
}

// One more committed row: the database's change counter moves, so the
// DbSetFingerprint does too.
inline void appendEngineDbRow(const fs::path &path, const std::string &trackPath)
{
    sqlite3 *db = nullptr;
    assert(sqlite3_open(seabass::pathToUtf8(path).c_str(), &db) == SQLITE_OK);
    const std::string sql = "INSERT INTO Track(path) VALUES('" + trackPath + "')";
    char *error = nullptr;
    assert(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) == SQLITE_OK);
    sqlite3_close(db);
}

// Covers the way a Denon player's rekordbox import writes them, into the
// m.db createEngineDb() made under `stickRoot`: `links` more tracks, each
// on a link to the stick as a player mounts it,
// "image://fileart//media/<label>/PIONEER/Artwork/00001/cover<n>.jpg",
// stored as a blob of text like Engine's own. The first `withImages` have
// their JPEG under PIONEER/Artwork on the stick; the rest point at an image
// that is not there. Returns the new tracks' ids, in order.
inline std::vector<std::int64_t> plantPlayerCoverLinks(const fs::path &stickRoot, const std::string &label, int links,
                                                       int withImages)
{
    sqlite3 *db = nullptr;
    assert(sqlite3_open(seabass::pathToUtf8(stickRoot / "Engine Library" / "Database2" / "m.db").c_str(), &db)
           == SQLITE_OK);
    assert(sqlite3_exec(db,
                        "ALTER TABLE Track ADD COLUMN albumArtId INTEGER; CREATE TABLE AlbumArt (id INTEGER PRIMARY "
                        "KEY AUTOINCREMENT, hash TEXT, albumArt BLOB);",
                        nullptr, nullptr, nullptr)
           == SQLITE_OK);
    std::vector<std::int64_t> tracks;
    for (int n = 0; n < links; ++n) {
        const std::string name = "cover" + std::to_string(n) + ".jpg";
        const std::string link = "image://fileart//media/" + label + "/PIONEER/Artwork/00001/" + name;
        sqlite3_stmt *stmt = nullptr;
        assert(sqlite3_prepare_v2(db, "INSERT INTO AlbumArt (hash) VALUES (?);", -1, &stmt, nullptr) == SQLITE_OK);
        sqlite3_bind_blob(stmt, 1, link.data(), static_cast<int>(link.size()), SQLITE_TRANSIENT);
        assert(sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
        const std::int64_t art = sqlite3_last_insert_rowid(db);
        const std::string sql = "INSERT INTO Track (path, albumArtId) VALUES ('Contents/linked" + std::to_string(n)
                                + ".mp3', " + std::to_string(art) + ");";
        assert(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
        tracks.push_back(sqlite3_last_insert_rowid(db));
        if (n < withImages) {
            writeFile(stickRoot / "PIONEER" / "Artwork" / "00001" / name,
                      std::string("\xFF\xD8\xFF", 3) + pseudoRandom(2'000, 100 + n), 1'700'000'200);
        }
    }
    sqlite3_close(db);
    return tracks;
}

// A track's cover row as stored: the hash's storage class and its bytes.
struct CoverRow
{
    int hashType = SQLITE_NULL;
    std::string hash;
};

inline CoverRow coverOf(const fs::path &stickRoot, std::int64_t trackId)
{
    sqlite3 *db = nullptr;
    assert(sqlite3_open_v2(seabass::pathToUtf8(stickRoot / "Engine Library" / "Database2" / "m.db").c_str(), &db,
                           SQLITE_OPEN_READONLY, nullptr)
           == SQLITE_OK);
    sqlite3_stmt *stmt = nullptr;
    assert(sqlite3_prepare_v2(db, "SELECT a.hash FROM Track t JOIN AlbumArt a ON a.id = t.albumArtId WHERE t.id = ?;",
                              -1, &stmt, nullptr)
           == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, trackId);
    CoverRow row;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        row.hashType = sqlite3_column_type(stmt, 0);
        const void *bytes = sqlite3_column_blob(stmt, 0);
        if (bytes != nullptr) {
            row.hash.assign(static_cast<const char *>(bytes), static_cast<std::size_t>(sqlite3_column_bytes(stmt, 0)));
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return row;
}

// Whether `row` is a cover kept in the library's own files: a 20-byte
// hash, and an image under Engine Library/Artwork named after it.
inline bool isStoredCoverFile(const fs::path &stickRoot, const CoverRow &row)
{
    if (row.hashType != SQLITE_BLOB || row.hash.size() != 20) {
        return false;
    }
    static constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string name;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const unsigned char c : row.hash) {
        buffer = (buffer << 8) | c;
        bits += 8;
        while (bits >= 6) {
            name += Alphabet[(buffer >> (bits - 6)) & 0x3F];
            bits -= 6;
        }
    }
    if (bits > 0) {
        name += Alphabet[(buffer << (6 - bits)) & 0x3F];
    }
    std::error_code ec;
    return fs::is_regular_file(stickRoot / "Engine Library" / "Artwork" / (name + ".jpg"), ec);
}

// path -> content for files, "<dir>" for directories, of everything the
// walker would back up.
inline std::map<std::string, std::string> snapshot(const fs::path &root)
{
    using namespace infrastructure::stick_backup;
    std::map<std::string, std::string> out;
    for (const TreeEntry &e : walkStickTree(root, application::CancellationToken::none()).entries) {
        out[e.relativePath] = e.isDirectory ? "<dir>" : readFile(root / seabass::pathFromUtf8(e.relativePath));
    }
    return out;
}

}  // namespace seabass::test_fixture
