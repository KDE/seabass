// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "engine_edit_times.hpp"

#include <sqlite3.h>

#include <filesystem>

namespace seabass::testing
{

std::optional<std::map<std::string, bool>> engineTrackDatedById(const std::string &engineLibraryRoot)
{
    const std::filesystem::path mainDb = std::filesystem::path(engineLibraryRoot) / "Database2" / "m.db";
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(mainDb.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return std::nullopt;
    }
    std::optional<std::map<std::string, bool>> dated;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT id, lastEditTime FROM Track", -1, &stmt, nullptr) == SQLITE_OK) {
        dated.emplace();
        int rc = SQLITE_ROW;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const bool set = sqlite3_column_type(stmt, 1) != SQLITE_NULL && sqlite3_column_int64(stmt, 1) > 0;
            (*dated)[std::to_string(sqlite3_column_int64(stmt, 0))] = set;
        }
        if (rc != SQLITE_DONE) {
            dated.reset();  // a read that stopped part-way is not the table
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return dated;
}

}  // namespace seabass::testing
