// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/engine_import_state.hpp"
#include "infrastructure/engine/engine_pending_journals.hpp"
#include "infrastructure/engine/engine_sqlite.hpp"
#include "infrastructure/paths/utf8_path.hpp"

#include <sqlite3.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace seabass::infrastructure::engine
{

namespace fs = std::filesystem;

namespace
{

fs::path engineDatabase(const std::string &engineLibraryPath)
{
    return pathFromUtf8(engineLibraryPath) / "Database2" / "m.db";
}

// export.pdb's header: four little-endian words in, after the leading
// zero, the page length, the table count and the next unused page, comes
// the sequence the whole file is stamped with. Read by hand rather than
// through the kaitai parser, which walks every table page to answer a
// question that lives in the first 24 bytes.
std::optional<std::uint64_t> librarySequenceOf(const std::string &pioneerPath)
{
    const fs::path pdb = pathFromUtf8(pioneerPath) / "rekordbox" / "export.pdb";
    std::ifstream in(pdb, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::array<std::uint32_t, 6> header{};
    in.read(reinterpret_cast<char *>(header.data()), sizeof(header));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(header))) {
        return std::nullopt;
    }
    return header[5];
}

}  // namespace

RekordboxImportState readRekordboxImportState(const std::string &engineLibraryPath, const std::string &pioneerPath)
{
    RekordboxImportState state;
    std::error_code ec;
    state.hasEngineLibrary = !engineLibraryPath.empty() && fs::is_regular_file(engineDatabase(engineLibraryPath), ec);
    const auto sequence = pioneerPath.empty() ? std::nullopt : librarySequenceOf(pioneerPath);
    state.hasRekordboxLibrary = sequence.has_value();
    state.librarySequence = sequence.value_or(0);
    if (!state.hasEngineLibrary) {
        return state;
    }
    // A journal left by a pulled stick would fail the read-only open
    // below with "readonly"; see engine_pending_journals.hpp (#48). A
    // database another connection is writing is left to the read below,
    // which, read-only, cannot roll anything back.
    try {
        recoverEnginePendingJournals(engineLibraryPath);
    } catch (const std::exception &e) {
        state.error = e.what();
        return state;
    }

    sqlite3 *handle = nullptr;
    if (sqlite3_open_v2(pathToUtf8(engineDatabase(engineLibraryPath)).c_str(), &handle, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) {
        state.error = "could not open the Engine database";
        if (handle != nullptr) {
            sqlite3_close(handle);
        }
        return state;
    }
    // A lock (another connection writing) or a journal still pending is a
    // read that failed, not a library without this column.
    const auto failedToRead = [&state, handle](int code) {
        const int primary = code & 0xff;
        if (primary != SQLITE_BUSY && primary != SQLITE_LOCKED && primary != SQLITE_READONLY) {
            return false;
        }
        state.error = std::string("could not read the Engine database: ") + sqlite3_errmsg(handle);
        return true;
    };
    sqlite3_stmt *stmt = nullptr;
    // Not "WHERE id = 1". Engine's own libraries number that row 1, and a
    // library libdjinterop created numbers it 2 (see the upstream reports
    // about Information's row id), so asking for a fixed id reads nothing
    // on half the libraries this app meets and reports them as having no
    // Engine library at all. There is one row either way.
    if (sqlite3_prepare_v2(handle,
                           "SELECT lastRekordBoxLibraryImportReadCounter FROM Information ORDER BY id LIMIT 1;", -1,
                           &stmt, nullptr)
        == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            state.engineCounter = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
        } else if (!failedToRead(sqlite3_extended_errcode(handle))) {
            // No Information row is an Engine library this app cannot
            // reason about, not a finding to report.
            state.hasEngineLibrary = false;
        }
    } else if (!failedToRead(sqlite3_extended_errcode(handle))) {
        // A schema without the column: an older Engine generation, which
        // does not have this prompt to suppress.
        state.hasEngineLibrary = false;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);
    return state;
}

std::map<std::string, std::int64_t> readEnginePdbImportKeys(const std::string &engineLibraryPath)
{
    const std::string database = pathToUtf8(engineDatabase(engineLibraryPath));
    sqlite3 *handle = nullptr;
    const auto fail = [&handle, &database](const std::string &what) {
        const std::string why = handle != nullptr ? sqlite3_errmsg(handle) : "out of memory";
        sqlite3_close(handle);
        throw std::runtime_error("could not read pdbImportKey from " + database + ": " + what + ": " + why);
    };
    if (sqlite3_open_v2(database.c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        fail("open");
    }
    std::map<std::string, std::int64_t> keys;
    bool present = false;
    try {
        present = hasColumn(handle, "Track", "pdbImportKey");
    } catch (const std::exception &) {
        fail("list the Track columns");
    }
    if (!present) {
        sqlite3_close(handle);
        return keys;
    }
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(handle, "SELECT id, coalesce(pdbImportKey, 0) FROM Track;", -1, &stmt, nullptr)
        != SQLITE_OK) {
        fail("query");
    }
    int step;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        keys[std::to_string(sqlite3_column_int64(stmt, 0))] = sqlite3_column_int64(stmt, 1);
    }
    sqlite3_finalize(stmt);
    if (step != SQLITE_DONE) {
        fail("read");
    }
    sqlite3_close(handle);
    return keys;
}

bool markRekordboxLibraryImported(const std::string &engineLibraryPath, std::uint64_t librarySequence,
                                  std::string *error, const std::function<void(const std::string &)> &beforeWrite,
                                  const std::string &databaseFileOverride)
{
    const fs::path database =
        databaseFileOverride.empty() ? engineDatabase(engineLibraryPath) : pathFromUtf8(databaseFileOverride);
    if (beforeWrite) {
        beforeWrite(pathToUtf8(database));
    }
    sqlite3 *handle = nullptr;
    if (sqlite3_open_v2(pathToUtf8(database).c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (error != nullptr) {
            *error = "could not open " + pathToUtf8(database);
        }
        if (handle != nullptr) {
            sqlite3_close(handle);
        }
        return false;
    }
    sqlite3_busy_timeout(handle, 5000);
    sqlite3_stmt *stmt = nullptr;
    // Every row, which is one row: see the read above for why naming an
    // id would be wrong.
    bool ok = sqlite3_prepare_v2(handle, "UPDATE Information SET lastRekordBoxLibraryImportReadCounter = ?;", -1,
                                 &stmt, nullptr)
        == SQLITE_OK;
    if (ok) {
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(librarySequence));
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    if (!ok && error != nullptr) {
        *error = std::string("could not write the Engine import counter: ") + sqlite3_errmsg(handle);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);
    return ok;
}

}  // namespace seabass::infrastructure::engine
