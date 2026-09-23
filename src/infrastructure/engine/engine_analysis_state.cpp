// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/engine_analysis_state.hpp"

#include <sqlite3.h>

#include <filesystem>

namespace seabass::infrastructure::engine
{

namespace fs = std::filesystem;

namespace
{

fs::path engineDatabase(const std::string &engineLibraryPath)
{
    return fs::path(engineLibraryPath) / "Database2" / "m.db";
}

}  // namespace

AnalysisStateAudit auditAnalysisState(const std::string &engineLibraryPath)
{
    AnalysisStateAudit audit;
    if (engineLibraryPath.empty()) {
        return audit;
    }
    std::error_code ec;
    const fs::path database = engineDatabase(engineLibraryPath);
    if (!fs::is_regular_file(database, ec)) {
        return audit;  // no Engine library here; not a finding
    }
    audit.libraryPresent = true;

    sqlite3 *handle = nullptr;
    if (sqlite3_open_v2(database.string().c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        audit.error = "could not open the Engine database";
        if (handle != nullptr) {
            sqlite3_close(handle);
        }
        return audit;
    }

    // Engine 1.x has no isAnalyzed column, and asking for it there is an
    // error rather than a zero. Probed the same way the library creator
    // probes it before writing, so both halves of this behaviour agree
    // about what "too old to say" means.
    //
    // A probe that does not RUN is a third thing again, and it is the
    // one this got wrong first. sqlite3_open_v2() is lazy: it does not
    // read the header, so a file that is not a database at all opens
    // without complaint and the pragma is what fails. Taking that for
    // "no such column" reported a corrupt library as merely an old one,
    // and an old one has nothing to report -- so an unreadable stick
    // came back reassuring. The probe failing is an error.
    // Is there a Track table at all? pragma_table_info() on a table that
    // does not exist returns no rows, so the column probe below counts
    // zero and reports "no isAnalyzed column" -- which the page reads as
    // an Engine 1.x library, i.e. nothing to worry about. A half-created
    // or empty m.db would therefore come back reassuring, which is the
    // exact failure the probe below was hardened against one step
    // earlier. Asked separately because the two answers are different
    // sentences.
    {
        sqlite3_stmt *table = nullptr;
        bool hasTrackTable = false;
        if (sqlite3_prepare_v2(handle,
                               "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='Track';", -1,
                               &table, nullptr)
                == SQLITE_OK
            && sqlite3_step(table) == SQLITE_ROW) {
            hasTrackTable = sqlite3_column_int(table, 0) == 1;
        } else {
            audit.error = std::string("could not read the Engine schema: ") + sqlite3_errmsg(handle);
        }
        sqlite3_finalize(table);
        if (!audit.error.empty() || !hasTrackTable) {
            if (audit.error.empty()) {
                audit.error = "this Engine database has no Track table";
            }
            sqlite3_close(handle);
            return audit;
        }
    }

    {
        sqlite3_stmt *shape = nullptr;
        const bool prepared =
            sqlite3_prepare_v2(handle, "SELECT count(*) FROM pragma_table_info('Track') WHERE name = 'isAnalyzed';",
                               -1, &shape, nullptr)
            == SQLITE_OK;
        if (prepared && sqlite3_step(shape) == SQLITE_ROW) {
            audit.hasColumn = sqlite3_column_int(shape, 0) == 1;
        } else {
            audit.error = std::string("could not read the Engine schema: ") + sqlite3_errmsg(handle);
        }
        sqlite3_finalize(shape);
    }
    if (!audit.error.empty() || !audit.hasColumn) {
        sqlite3_close(handle);
        return audit;
    }

    // Both numbers from one pass, so the total and the count can never
    // come from two different reads of a database something else is
    // writing. A NULL isAnalyzed counts as not analysed: the player has
    // no result recorded either way.
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(handle,
                           "SELECT count(*), sum(CASE WHEN isAnalyzed IS NULL OR isAnalyzed = 0 THEN 1 ELSE 0 END) "
                           "FROM Track;",
                           -1, &stmt, nullptr)
            == SQLITE_OK
        && sqlite3_step(stmt) == SQLITE_ROW) {
        audit.tracksChecked = sqlite3_column_int(stmt, 0);
        audit.notAnalyzed = sqlite3_column_int(stmt, 1);
    } else {
        audit.error = "could not read the Engine track table";
    }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);
    return audit;
}

}  // namespace seabass::infrastructure::engine
