// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <sqlite3.h>

#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/sqlite_pending_journal.hpp"

namespace seabass::infrastructure::engine
{

static_assert(SQLITE_READONLY_ROLLBACK == pending_journal_codes::readOnlyRollback);
static_assert(SQLITE_BUSY == pending_journal_codes::busy && SQLITE_LOCKED == pending_journal_codes::locked);

// Engine's databases after a stick was pulled mid-save: every one with a
// hot journal is rolled back before anything reads it, with a copy kept
// on this computer first. See sqlite_pending_journal.hpp (#48).
// Throws when a hot journal cannot be rolled back, or a pending one cannot
// be checked, saying so, rather than letting the read that follows fail
// with SQLite's "readonly" or roll it back without a copy.
// Returns the name of a database another connection kept writing for all
// of `busyWait`, left unchecked, or an empty string. A caller that goes on
// to open the library read-write must not do so then.
inline std::string recoverEnginePendingJournals(const std::string &engineLibraryPath,
                                                std::chrono::milliseconds busyWait = std::chrono::milliseconds(0))
{
    const std::filesystem::path database2 = pathFromUtf8(engineLibraryPath) / "Database2";
    std::string busy;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(database2, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".db") {
            continue;
        }
        const std::filesystem::path db = entry.path();
        const auto openAndRead = [&db](int flags) {
            sqlite3 *handle = nullptr;
            SqliteReadOutcome outcome;
            if (sqlite3_open_v2(pathToUtf8(db).c_str(), &handle, flags, nullptr) != SQLITE_OK) {
                outcome.code = handle != nullptr ? sqlite3_extended_errcode(handle) : SQLITE_CANTOPEN;
                outcome.message = handle != nullptr ? sqlite3_errmsg(handle) : "could not open";
            } else if (sqlite3_exec(handle, "SELECT count(*) FROM sqlite_master", nullptr, nullptr, nullptr)
                       != SQLITE_OK) {
                outcome.code = sqlite3_extended_errcode(handle);
                outcome.message = sqlite3_errmsg(handle);
            }
            sqlite3_close(handle);
            return outcome;
        };
        const PendingJournalRecovery recovery =
            recoverPendingJournal(db, paths::localRoot() / "recovered",
                                  [&openAndRead]() { return openAndRead(SQLITE_OPEN_READONLY); },
                                  [&openAndRead]() { return openAndRead(SQLITE_OPEN_READWRITE); }, busyWait);
        if (recovery.found && !recovery.recovered) {
            throw std::runtime_error("the Engine Library on this stick was left mid-save (was the stick pulled while "
                                     "saving?) and " + pathToUtf8(db.filename()) + " could not be put back: "
                                     + recovery.error
                                     + ". If the stick is mounted read-only, repair it in Library Health first.");
        }
        if (recovery.busy) {
            if (busy.empty()) {
                busy = pathToUtf8(db.filename());
            }
        } else if (!recovery.found && !recovery.error.empty()) {
            throw std::runtime_error("the Engine Library on this stick has an unfinished save beside "
                                     + pathToUtf8(db.filename()) + " that could not be checked: " + recovery.error);
        }
        if (recovery.recovered) {
            std::cerr << "engine: rolled back an unfinished save in " << pathToUtf8(db)
                      << "; a copy of how it was is in " << pathToUtf8(recovery.keptCopy) << "\n";
        }
    }
    return busy;
}

}  // namespace seabass::infrastructure::engine
