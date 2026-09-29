// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <sqlite3.h>

#include <stdexcept>
#include <string>
#include <string_view>

// Small SQLite helpers shared by the code that reads an Engine m.db
// directly. Only for translation units that already use plain SQLite: a
// unit that includes SQLCipher's header cannot include this one.
namespace seabass::infrastructure::engine
{

// Whether `table` has `column`, for the schema differences between Engine
// versions. Throws when the columns cannot be listed, so a failed read is
// never taken for a column that is not there.
inline bool hasColumn(sqlite3 *handle, std::string_view table, std::string_view column)
{
    sqlite3_stmt *columns = nullptr;
    const std::string sql = "PRAGMA table_info(" + std::string(table) + ");";
    if (sqlite3_prepare_v2(handle, sql.c_str(), -1, &columns, nullptr) != SQLITE_OK) {
        throw std::runtime_error("could not list the columns of " + std::string(table) + ": " + sqlite3_errmsg(handle));
    }
    bool found = false;
    int step;
    while ((step = sqlite3_step(columns)) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(columns, 1);
        found = found || (name != nullptr && std::string_view(reinterpret_cast<const char *>(name)) == column);
    }
    sqlite3_finalize(columns);
    if (step != SQLITE_DONE) {
        throw std::runtime_error("could not list the columns of " + std::string(table) + ": " + sqlite3_errmsg(handle));
    }
    return found;
}

// SQL for the size in bytes of an image column. length() counts a text
// value in characters, up to its first NUL, and casting to a blob loads
// the whole value, so the cast is taken for text alone and a blob keeps
// SQLite's cheap length.
inline std::string byteLengthSql(std::string_view column)
{
    const std::string c(column);
    return "(CASE typeof(" + c + ") WHEN 'text' THEN length(CAST(" + c + " AS BLOB)) ELSE length(" + c + ") END)";
}

}  // namespace seabass::infrastructure::engine
