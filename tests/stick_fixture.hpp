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

#include "application/ports/cancellation_token.hpp"
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
    assert(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
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
    assert(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    const std::string sql = "INSERT INTO Track(path) VALUES('" + trackPath + "')";
    char *error = nullptr;
    assert(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) == SQLITE_OK);
    sqlite3_close(db);
}

// path -> content for files, "<dir>" for directories, of everything the
// walker would back up.
inline std::map<std::string, std::string> snapshot(const fs::path &root)
{
    using namespace infrastructure::stick_backup;
    std::map<std::string, std::string> out;
    for (const TreeEntry &e : walkStickTree(root, application::CancellationToken::none()).entries) {
        out[e.relativePath] = e.isDirectory ? "<dir>" : readFile(root / pathFromUtf8(e.relativePath));
    }
    return out;
}

}  // namespace seabass::test_fixture
