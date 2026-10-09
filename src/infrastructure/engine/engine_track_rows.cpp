// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/engine_track_rows.hpp"

#include <set>
#include <stdexcept>

#include <sqlite3.h>

namespace seabass::infrastructure::engine
{

namespace
{

// One connection, closed on every way out.
class Connection
{
public:
    Connection(const std::string &file, int flags)
    {
        m_ok = sqlite3_open_v2(file.c_str(), &m_handle, flags, nullptr) == SQLITE_OK;
        if (m_ok) {
            sqlite3_busy_timeout(m_handle, 5000);
        } else {
            m_why = m_handle ? sqlite3_errmsg(m_handle) : "out of memory";
        }
    }
    ~Connection() { sqlite3_close(m_handle); }
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    bool ok() const { return m_ok; }
    const std::string &why() const { return m_why; }
    sqlite3 *handle() const { return m_handle; }

    bool exec(const std::string &sql, std::string *error) const
    {
        char *message = nullptr;
        if (sqlite3_exec(m_handle, sql.c_str(), nullptr, nullptr, &message) == SQLITE_OK) {
            return true;
        }
        *error = sql + ": " + (message ? message : "unknown error");
        sqlite3_free(message);
        return false;
    }

    // Runs `sql` with each id bound to ?1 in turn and returns the first
    // column of its row (a count) or the number of rows it changed; -1 on
    // an error, with *error set.
    std::int64_t count(const std::string &sql, std::int64_t id, std::string *error) const
    {
        return run(sql, id, true, error);
    }
    std::int64_t change(const std::string &sql, std::int64_t id, std::string *error) const
    {
        return run(sql, id, false, error);
    }

private:
    std::int64_t run(const std::string &sql, std::int64_t id, bool readsRow, std::string *error) const
    {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(m_handle, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            *error = sql + ": " + sqlite3_errmsg(m_handle);
            sqlite3_finalize(stmt);
            return -1;
        }
        sqlite3_bind_int64(stmt, 1, id);
        const int step = sqlite3_step(stmt);
        std::int64_t result = -1;
        if (readsRow && step == SQLITE_ROW) {
            result = sqlite3_column_int64(stmt, 0);
        } else if (!readsRow && step == SQLITE_DONE) {
            result = sqlite3_changes(m_handle);
        } else {
            *error = sql + ": " + sqlite3_errmsg(m_handle);
        }
        sqlite3_finalize(stmt);
        return result;
    }

    sqlite3 *m_handle = nullptr;
    bool m_ok = false;
    std::string m_why;
};

// Every table (not view) with a trackId column, in the order sqlite_master
// lists them. Empty with *error set when the schema cannot be read.
std::vector<std::string> tablesWithTrackId(const Connection &db, std::string *error)
{
    std::vector<std::string> tables;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db.handle(), "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name;", -1, &stmt,
                           nullptr)
        != SQLITE_OK) {
        *error = std::string("could not read the schema: ") + sqlite3_errmsg(db.handle());
        sqlite3_finalize(stmt);
        return {};
    }
    std::vector<std::string> all;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto *name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
        all.emplace_back(name ? name : "");
    }
    sqlite3_finalize(stmt);
    for (const std::string &table : all) {
        // pragma_table_info with the name bound, rather than spliced into
        // PRAGMA text: the name comes from the file.
        if (sqlite3_prepare_v2(db.handle(), "SELECT count(*) FROM pragma_table_info(?1) WHERE name = 'trackId';", -1,
                               &stmt, nullptr)
            != SQLITE_OK) {
            *error = std::string("could not read the schema: ") + sqlite3_errmsg(db.handle());
            sqlite3_finalize(stmt);
            return {};
        }
        sqlite3_bind_text(stmt, 1, table.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) > 0) {
            tables.push_back(table);
        }
        sqlite3_finalize(stmt);
    }
    return tables;
}

bool contains(const std::vector<std::string> &names, const std::string &name)
{
    for (const auto &n : names) {
        if (n == name) {
            return true;
        }
    }
    return false;
}

std::string quoted(const std::string &identifier)
{
    std::string out = "\"";
    for (const char c : identifier) {
        out += c;
        if (c == '"') {
            out += '"';
        }
    }
    return out + "\"";
}

}  // namespace

int removeEngineTrackRows(const std::string &databaseFile, const std::vector<std::int64_t> &trackIds, std::string *error)
{
    if (error == nullptr) {
        throw std::invalid_argument("removeEngineTrackRows: the error out-parameter is required");
    }
    error->clear();
    if (trackIds.empty()) {
        return 0;
    }
    std::set<std::int64_t> seen;
    for (const std::int64_t id : trackIds) {
        if (!seen.insert(id).second) {
            *error = "track id=" + std::to_string(id) + " is named twice";
            return -1;
        }
    }

    Connection db(databaseFile, SQLITE_OPEN_READWRITE);
    if (!db.ok()) {
        *error = "could not open " + databaseFile + ": " + db.why();
        return -1;
    }
    if (!db.exec("BEGIN IMMEDIATE;", error)) {
        return -1;
    }
    const auto fail = [&]() {
        std::string ignored;
        db.exec("ROLLBACK;", &ignored);
        return -1;
    };

    const std::vector<std::string> named = tablesWithTrackId(db, error);
    if (!error->empty()) {
        return fail();
    }
    if (!contains(named, "PlaylistEntity")) {
        // Every Engine 2.x and 3.x schema has it; without it this is not
        // a database this function knows how to edit.
        *error = databaseFile + " has no PlaylistEntity table with a trackId; not an Engine 2.x or 3.x database";
        return fail();
    }

    // The playlist entries first, so Engine's trigger relinks each chain
    // while the rest of the list is still intact; Track last.
    std::vector<std::string> statements;
    for (const char *table : {"PlaylistEntity", "PreparelistEntity", "PerformanceData"}) {
        if (contains(named, table)) {
            statements.push_back(std::string("DELETE FROM ") + table + " WHERE trackId = ?1;");
        }
    }
    if (contains(named, "ChangeLog")) {
        statements.emplace_back("UPDATE ChangeLog SET trackId = NULL WHERE trackId = ?1;");
    }

    int removed = 0;
    for (const std::int64_t id : trackIds) {
        const std::int64_t present = db.count("SELECT count(*) FROM Track WHERE id = ?1;", id, error);
        if (present < 0) {
            return fail();
        }
        if (present == 0) {
            *error = "no track id=" + std::to_string(id) + " in " + databaseFile;
            return fail();
        }
        for (const std::string &sql : statements) {
            if (db.change(sql, id, error) < 0) {
                return fail();
            }
        }
        const std::int64_t gone = db.change("DELETE FROM Track WHERE id = ?1;", id, error);
        if (gone < 0) {
            return fail();
        }
        if (gone != 1) {
            *error = "deleting track id=" + std::to_string(id) + " removed " + std::to_string(gone) + " rows";
            return fail();
        }
        ++removed;
    }

    // Nothing may still name a removed track. Every table Engine is known
    // to keep one in was handled above, so this fires only on a schema
    // with a table this function has not met: refused, not left dangling.
    for (const std::int64_t id : trackIds) {
        for (const std::string &table : named) {
            const std::int64_t left =
                db.count("SELECT count(*) FROM " + quoted(table) + " WHERE trackId = ?1;", id, error);
            if (left < 0) {
                return fail();
            }
            if (left > 0) {
                *error = std::to_string(left) + " row(s) in " + table + " still name track id=" + std::to_string(id)
                    + " after removing it; not removing anything";
                return fail();
            }
        }
    }

    if (!db.exec("COMMIT;", error)) {
        return fail();
    }
    return removed;
}

int engineRowsNamingTrack(const std::string &databaseFile, std::int64_t trackId, std::string *error)
{
    if (error == nullptr) {
        throw std::invalid_argument("engineRowsNamingTrack: the error out-parameter is required");
    }
    error->clear();
    Connection db(databaseFile, SQLITE_OPEN_READONLY);
    if (!db.ok()) {
        *error = "could not open " + databaseFile + ": " + db.why();
        return -1;
    }
    const std::vector<std::string> named = tablesWithTrackId(db, error);
    if (!error->empty()) {
        return -1;
    }
    std::int64_t total = db.count("SELECT count(*) FROM Track WHERE id = ?1;", trackId, error);
    if (total < 0) {
        return -1;
    }
    for (const std::string &table : named) {
        const std::int64_t rows = db.count("SELECT count(*) FROM " + quoted(table) + " WHERE trackId = ?1;", trackId, error);
        if (rows < 0) {
            return -1;
        }
        total += rows;
    }
    return static_cast<int>(total);
}

}  // namespace seabass::infrastructure::engine
