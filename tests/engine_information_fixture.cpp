// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "engine_information_fixture.hpp"

#include <sqlite3.h>

#include <stdexcept>
#include <string>

namespace seabass::testing
{

void createEngineInformation(const std::filesystem::path &databaseFile, std::int64_t counter, int rowId)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open(databaseFile.string().c_str(), &db) != SQLITE_OK) {
        throw std::runtime_error("cannot create " + databaseFile.string());
    }
    const std::string sql = "CREATE TABLE Information (id INTEGER PRIMARY KEY, lastRekordBoxLibraryImportReadCounter "
                            "INTEGER); INSERT INTO Information VALUES ("
        + std::to_string(rowId) + ", " + std::to_string(counter) + ");";
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("cannot write Information in " + databaseFile.string());
    }
}

std::int64_t readEngineImportCounter(const std::filesystem::path &databaseFile)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(databaseFile.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *stmt = nullptr;
    std::int64_t value = -1;
    if (sqlite3_prepare_v2(db, "SELECT lastRekordBoxLibraryImportReadCounter FROM Information", -1, &stmt, nullptr)
            == SQLITE_OK
        && sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return value;
}

}  // namespace seabass::testing
