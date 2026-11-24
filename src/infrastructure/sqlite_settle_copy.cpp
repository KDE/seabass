// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// settleReadCopy() (database_read_copy.hpp): in its own file, because it
// needs <sqlite3.h>, which the SQLCipher side that includes that header
// cannot have beside its own declarations.

#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "infrastructure/database_read_copy.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::infrastructure
{

namespace
{

bool isPlainSqlite(const std::filesystem::path &db)
{
    std::ifstream in(db, std::ios::binary);
    char header[16] = {};
    return in.read(header, sizeof header) && std::string_view(header, 16) == std::string_view("SQLite format 3\0", 16);
}

}  // namespace

void settleReadCopy(const std::filesystem::path &copy)
{
    if (!isPlainSqlite(copy)) {
        return;  // encrypted: read read-only by all its readers as it is
    }
    sqlite3 *handle = nullptr;
    const auto fail = [&handle, &copy](const std::string &what) {
        const std::string why = handle ? sqlite3_errmsg(handle) : "out of memory";
        sqlite3_close(handle);
        throw std::runtime_error("could not make the copy " + pathToUtf8(copy) + " one file (" + what + "): " + why);
    };
    if (sqlite3_open_v2(pathToUtf8(copy).c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        fail("open");
    }
    // The first read rolls back a pending journal; the mode change folds
    // the -wal into the database and deletes it.
    if (sqlite3_exec(handle, "PRAGMA journal_mode=DELETE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        fail("journal mode");
    }
    if (sqlite3_close(handle) != SQLITE_OK) {
        handle = nullptr;
        throw std::runtime_error("could not close the copy " + pathToUtf8(copy));
    }
}

}  // namespace seabass::infrastructure
