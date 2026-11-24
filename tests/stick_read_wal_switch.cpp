// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// stick_read_leaves_stick_alone_test's plain SQLite half: an Engine m.db
// switched to WAL mode.

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "infrastructure/paths/utf8_path.hpp"

namespace fs = std::filesystem;
using seabass::pathToUtf8;

namespace
{

void exec(sqlite3 *db, const std::string &sql)
{
    char *message = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
        std::cerr << sql << ": " << (message ? message : "") << "\n";
        sqlite3_free(message);
        std::exit(2);
    }
}

}  // namespace

// The stick's m.db switched to WAL mode. With `rowsInWal`, a -wal and
// -shm are left beside it holding a committed change, as a player that
// lost power mid-session leaves them: the change is made in a staging copy
// and its three files copied over while the connection is still open.
bool switchEngineToWal(const fs::path &database2, const fs::path &staging, bool rowsInWal)
{
    fs::remove_all(staging);
    fs::create_directories(staging);
    const fs::path db = staging / "m.db";
    fs::copy_file(database2 / "m.db", db);
    sqlite3 *handle = nullptr;
    if (sqlite3_open_v2(pathToUtf8(db).c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        std::cerr << "could not open " << pathToUtf8(db) << "\n";
        std::exit(2);
    }
    exec(handle, "PRAGMA journal_mode=WAL;");
    if (rowsInWal) {
        exec(handle, "PRAGMA wal_autocheckpoint=0;");
        exec(handle, "CREATE TABLE seabass_wal_probe (id INTEGER PRIMARY KEY); INSERT INTO seabass_wal_probe VALUES (1);");
        for (const char *suffix : {"", "-wal", "-shm"}) {
            fs::copy_file(staging / (std::string("m.db") + suffix), database2 / (std::string("m.db") + suffix),
                          fs::copy_options::overwrite_existing);
        }
    }
    sqlite3_close(handle);
    if (!rowsInWal) {
        fs::copy_file(db, database2 / "m.db", fs::copy_options::overwrite_existing);
    }
    std::error_code ec;
    return rowsInWal == fs::exists(database2 / "m.db-wal", ec);
}


int walProbeRows(const fs::path &db)
{
    sqlite3 *handle = nullptr;
    int rows = -1;
    if (sqlite3_open_v2(pathToUtf8(db).c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(handle, "SELECT count(*) FROM seabass_wal_probe;", -1, &stmt, nullptr) == SQLITE_OK
            && sqlite3_step(stmt) == SQLITE_ROW) {
            rows = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(handle);
    return rows;
}
