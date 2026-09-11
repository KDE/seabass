// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <sqlite3.h>

#include <ctime>
#include <optional>
#include <stdexcept>
#include <string>

namespace seabass::infrastructure::local
{

// The small amount of SQLite ceremony every local store repeats: an RAII
// statement, a throwing exec(), and a UTC timestamp.
//
// Header-only and shared rather than copied per store. There were two
// byte-identical copies of this in the tree the moment a second SQLite
// store existed, which is the point at which path_key.hpp's warning
// applies -- a third is where one of them quietly drifts.
//
// Every error message is prefixed with a caller-supplied context so a
// thrown message says which store failed, not just that sqlite did.

// Current UTC time as ISO 8601, e.g. "2026-09-10T00:35:12Z".
inline std::string isoTimestampUtc()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    // gmtime_s takes its arguments in the opposite order from POSIX's
    // gmtime_r (destination first) and returns errno_t rather than a
    // struct tm* -- not just a rename.
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// Thin RAII wrapper so a thrown exception (or an early return) never
// leaks a prepared statement.
class Statement
{
public:
    Statement(sqlite3 *db, const char *sql, const char *context) : m_db(db), m_context(context)
    {
        if (sqlite3_prepare_v2(db, sql, -1, &m_stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string(m_context) + ": failed to prepare statement: " +
                                      sqlite3_errmsg(db));
        }
    }
    ~Statement() { sqlite3_finalize(m_stmt); }

    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    void bind(int index, const std::string &value)
    {
        sqlite3_bind_text(m_stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
    }
    void bind(int index, double value) { sqlite3_bind_double(m_stmt, index, value); }
    void bind(int index, int value) { sqlite3_bind_int(m_stmt, index, value); }
    void bindInt64(int index, sqlite3_int64 value) { sqlite3_bind_int64(m_stmt, index, value); }
    void bindBlob(int index, const std::string &value)
    {
        sqlite3_bind_blob(m_stmt, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    }
    void bindNull(int index) { sqlite3_bind_null(m_stmt, index); }
    // An absent optional binds SQL NULL rather than 0. The distinction
    // matters for domain::Track::rating, where 0 is a real rating and
    // "unrated" is a different fact.
    void bind(int index, const std::optional<int> &value)
    {
        if (value) {
            sqlite3_bind_int(m_stmt, index, *value);
        } else {
            sqlite3_bind_null(m_stmt, index);
        }
    }

    // Runs to completion; throws if the statement reports an error.
    void run()
    {
        int rc = sqlite3_step(m_stmt);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            throw std::runtime_error(std::string(m_context) + ": statement failed: " + sqlite3_errmsg(m_db));
        }
    }

    // For SELECTs: advances to the next row, returning false once
    // exhausted.
    bool step()
    {
        int rc = sqlite3_step(m_stmt);
        if (rc == SQLITE_ROW) {
            return true;
        }
        if (rc == SQLITE_DONE) {
            return false;
        }
        throw std::runtime_error(std::string(m_context) + ": statement failed: " + sqlite3_errmsg(m_db));
    }

    void reset()
    {
        sqlite3_reset(m_stmt);
        sqlite3_clear_bindings(m_stmt);
    }

    std::string columnText(int index)
    {
        const unsigned char *text = sqlite3_column_text(m_stmt, index);
        return text ? reinterpret_cast<const char *>(text) : "";
    }
    double columnDouble(int index) { return sqlite3_column_double(m_stmt, index); }
    int columnInt(int index) { return sqlite3_column_int(m_stmt, index); }
    sqlite3_int64 columnInt64(int index) { return sqlite3_column_int64(m_stmt, index); }
    bool columnIsNull(int index) { return sqlite3_column_type(m_stmt, index) == SQLITE_NULL; }
    std::optional<int> columnOptionalInt(int index)
    {
        if (columnIsNull(index)) {
            return std::nullopt;
        }
        return sqlite3_column_int(m_stmt, index);
    }
    std::string columnBlob(int index)
    {
        const void *data = sqlite3_column_blob(m_stmt, index);
        int size = sqlite3_column_bytes(m_stmt, index);
        return data ? std::string(reinterpret_cast<const char *>(data), static_cast<size_t>(size)) : std::string();
    }

    sqlite3_int64 lastInsertRowId() { return sqlite3_last_insert_rowid(m_db); }

private:
    sqlite3 *m_db;
    const char *m_context;
    sqlite3_stmt *m_stmt = nullptr;
};

inline void exec(sqlite3 *db, const char *sql, const char *context)
{
    char *errMsg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::string message = errMsg ? errMsg : "unknown error";
        sqlite3_free(errMsg);
        throw std::runtime_error(std::string(context) + ": " + message);
    }
}

}  // namespace seabass::infrastructure::local
