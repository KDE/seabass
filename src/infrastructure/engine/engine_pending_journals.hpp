// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

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

// Engine's databases after a stick was pulled mid-save: every one with a
// pending journal is rolled back before anything reads it read-only, with a
// copy kept on this computer first. See sqlite_pending_journal.hpp (#48).
// Throws when a pending journal cannot be rolled back, saying so, rather
// than letting the read that follows fail with SQLite's "readonly".
inline void recoverEnginePendingJournals(const std::string &engineLibraryPath)
{
    const std::filesystem::path database2 = pathFromUtf8(engineLibraryPath) / "Database2";
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(database2, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".db") {
            continue;
        }
        const std::filesystem::path db = entry.path();
        const auto openAndRead = [&db](int flags) {
                sqlite3 *handle = nullptr;
                const int opened = sqlite3_open_v2(pathToUtf8(db).c_str(), &handle, flags, nullptr);
                const std::string openError = handle != nullptr ? sqlite3_errmsg(handle) : "could not open";
                int read = SQLITE_ERROR;
                std::string readError;
                if (opened == SQLITE_OK) {
                    read = sqlite3_exec(handle, "SELECT count(*) FROM sqlite_master", nullptr, nullptr, nullptr);
                    readError = sqlite3_errmsg(handle);
                }
                sqlite3_close(handle);
                if (opened != SQLITE_OK) {
                    throw std::runtime_error(openError);
                }
                if (read != SQLITE_OK) {
                    throw std::runtime_error(readError);
                }
        };
        const PendingJournalRecovery recovery =
            recoverPendingJournal(db, paths::localRoot() / "recovered",
                                  [&openAndRead]() { openAndRead(SQLITE_OPEN_READONLY); },
                                  [&openAndRead]() { openAndRead(SQLITE_OPEN_READWRITE); });
        if (recovery.found && !recovery.recovered) {
            throw std::runtime_error("the Engine Library on this stick was left mid-save (was the stick pulled while "
                                     "saving?) and " + pathToUtf8(db.filename()) + " could not be put back: "
                                     + recovery.error
                                     + ". If the stick is mounted read-only, repair it in Library Health first.");
        }
        if (recovery.recovered) {
            std::cerr << "engine: rolled back an unfinished save in " << pathToUtf8(db)
                      << "; a copy of how it was is in " << pathToUtf8(recovery.keptCopy) << "\n";
        }
    }
}

}  // namespace seabass::infrastructure::engine
