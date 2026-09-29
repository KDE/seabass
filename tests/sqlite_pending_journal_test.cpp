// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// recoverPendingJournal(), against a real SQLite crash state (#48). The
// macOS round 8 check P3 pulled a stick mid-save: OneLibrary was left with
// a hot journal, and every read-only open of it failed with "attempt to
// write a readonly database". Here the same state is made on purpose -- a
// transaction whose changes have already reached the database file, copied
// with its journal before it commits -- and put back.

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <sqlite3.h>

#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/sqlite_pending_journal.hpp"

namespace fs = std::filesystem;
using seabass::infrastructure::hasPendingJournal;
using seabass::infrastructure::recoverPendingJournal;

namespace
{

void exec(sqlite3 *db, const std::string &sql)
{
    char *error = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error != nullptr ? error : "?";
        sqlite3_free(error);
        throw std::runtime_error(sql + ": " + message);
    }
}

// SUM(v) through a read-only connection, or -1 when it cannot be read.
long long readOnlySum(const fs::path &path, std::string *errorOut = nullptr)
{
    sqlite3 *db = nullptr;
    long long sum = -1;
    if (sqlite3_open_v2(seabass::pathToUtf8(path).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT SUM(v) FROM t", -1, &stmt, nullptr) == SQLITE_OK
            && sqlite3_step(stmt) == SQLITE_ROW) {
            sum = sqlite3_column_int64(stmt, 0);
        } else if (errorOut != nullptr) {
            *errorOut = sqlite3_errmsg(db);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return sum;
}

seabass::infrastructure::SqliteReadOutcome openAndRead(const fs::path &path, int flags)
{
    seabass::infrastructure::SqliteReadOutcome outcome;
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(seabass::pathToUtf8(path).c_str(), &db, flags, nullptr) != SQLITE_OK
        || sqlite3_exec(db, "SELECT count(*) FROM sqlite_master", nullptr, nullptr, nullptr) != SQLITE_OK) {
        outcome.code = db != nullptr ? sqlite3_extended_errcode(db) : SQLITE_CANTOPEN;
        outcome.message = db != nullptr ? sqlite3_errmsg(db) : "could not open";
    }
    sqlite3_close(db);
    return outcome;
}
seabass::infrastructure::SqliteReadOutcome openReadOnlyAndRead(const fs::path &path)
{
    return openAndRead(path, SQLITE_OPEN_READONLY);
}
seabass::infrastructure::SqliteReadOutcome openReadWriteAndRead(const fs::path &path)
{
    return openAndRead(path, SQLITE_OPEN_READWRITE);
}

// A database of 2000 rows summing to 2000, and a copy of it taken in the
// middle of a transaction that sets every row to 7 -- with the database
// file already changed, as a pull mid-commit leaves it.
fs::path makeInterrupted(const fs::path &dir)
{
    const fs::path live = dir / "live.db";
    sqlite3 *db = nullptr;
    sqlite3_open(seabass::pathToUtf8(live).c_str(), &db);
    exec(db, "PRAGMA journal_mode=DELETE");
    exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER, pad TEXT)");
    exec(db, "BEGIN");
    for (int i = 0; i < 2000; ++i) {
        exec(db, "INSERT INTO t(v, pad) VALUES (1, hex(randomblob(200)))");
    }
    exec(db, "COMMIT");
    // Spill after one page: the update has to write changed pages into the
    // database file long before COMMIT, which syncs the journal and gives
    // it its live header first -- so the copy below holds changed pages
    // and a hot journal with the originals. cache_spill has its own
    // threshold (20000 pages by default); cache_size alone spills nothing.
    exec(db, "PRAGMA cache_size=1");
    exec(db, "PRAGMA cache_spill=1");
    exec(db, "BEGIN");
    exec(db, "UPDATE t SET v = 7");

    const fs::path crashed = dir / "crashed.db";
    fs::copy_file(live, crashed);
    fs::copy_file(dir / "live.db-journal", dir / "crashed.db-journal");
    exec(db, "ROLLBACK");
    sqlite3_close(db);
    return crashed;
}

}  // namespace

int main()
{
    const fs::path root = fs::temp_directory_path()
        / seabass::pathFromUtf8("seabass-pending-journal-"
                                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(root);
    fs::create_directories(root);

    // 1. The state a pull leaves, and the failure it caused.
    {
        const fs::path crashed = makeInterrupted(root);
        assert(hasPendingJournal(crashed) && "the copied journal is live");
        std::string error;
        assert(readOnlySum(crashed, &error) == -1 && "a read-only open cannot get past a hot journal");
        std::cout << "case 1 (a read-only open fails: " << error << ") OK\n";
    }

    // 2. Recovered: the database is back to before the transaction, the
    //    journal is spent, and both files were kept first.
    {
        const fs::path crashed = root / "crashed.db";
        const auto recovery = recoverPendingJournal(crashed, root / "recovered",
                                                    [&crashed]() { return openReadOnlyAndRead(crashed); },
                                                    [&crashed]() { return openReadWriteAndRead(crashed); });
        assert(recovery.found && recovery.recovered && recovery.error.empty());
        assert(!hasPendingJournal(crashed));
        assert(readOnlySum(crashed) == 2000 && "every row is back to 1: the unfinished update is gone");
        assert(fs::exists(recovery.keptCopy / "crashed.db") && fs::exists(recovery.keptCopy / "crashed.db-journal"));
        assert(hasPendingJournal(recovery.keptCopy / "crashed.db") && "the kept copy is the state as found");
        std::cout << "case 2 (rolled back, copy kept) OK\n";
    }

    // 3. No journal: nothing done, nothing copied.
    {
        const fs::path crashed = root / "crashed.db";
        bool opened = false;
        const auto recovery = recoverPendingJournal(
            crashed, root / "recovered-again",
            [&opened]() { opened = true; return seabass::infrastructure::SqliteReadOutcome{}; },
            [&opened]() { opened = true; return seabass::infrastructure::SqliteReadOutcome{}; });
        assert(!recovery.found && !recovery.recovered && !opened);
        assert(!fs::exists(root / "recovered-again"));
        std::cout << "case 3 (nothing pending, nothing touched) OK\n";
    }

    // 4. The open for writing fails (a stick mounted read-only): not
    //    recovered, said so, and the stick's files left as they were.
    {
        const fs::path second = root / "second";
        fs::create_directories(second);
        const fs::path crashed = makeInterrupted(second);
        const auto recovery = recoverPendingJournal(crashed, root / "recovered-3",
            [&crashed]() { return openReadOnlyAndRead(crashed); },
            []() { return seabass::infrastructure::SqliteReadOutcome{SQLITE_READONLY, "attempt to write a readonly database"}; });
        assert(recovery.found && !recovery.recovered);
        assert(recovery.error.find("readonly") != std::string::npos);
        assert(hasPendingJournal(crashed) && "nothing was rolled back");
        std::cout << "case 4 (cannot write: reported, left alone) OK\n";
    }

    // 5. A writer mid-commit leaves a live journal too, but holds the lock:
    //    the read-only read meets SQLITE_BUSY, not a hot journal. Nothing
    //    copied, nothing rolled back, reported busy.
    {
        const fs::path dir = root / "busy";
        fs::create_directories(dir);
        makeInterrupted(dir);
        const fs::path live = dir / "live.db";
        sqlite3 *writer = nullptr;
        sqlite3_open(seabass::pathToUtf8(live).c_str(), &writer);
        exec(writer, "PRAGMA cache_size=1");
        exec(writer, "PRAGMA cache_spill=1");
        exec(writer, "BEGIN");
        exec(writer, "UPDATE t SET v = 9");
        assert(hasPendingJournal(live) && "the writer's journal is live");
        bool wrote = false;
        const auto recovery = recoverPendingJournal(
            live, root / "recovered-busy", [&live]() { return openReadOnlyAndRead(live); },
            [&live, &wrote]() { wrote = true; return openReadWriteAndRead(live); }, std::chrono::milliseconds(100));
        assert(recovery.busy && !recovery.found && !recovery.recovered && !wrote);
        assert(!fs::exists(root / "recovered-busy") && "nothing copied");
        exec(writer, "COMMIT");
        sqlite3_close(writer);
        assert(readOnlySum(live) == 9 * 2000 && "the writer's commit stands");
        std::cout << "case 5 (a live writer: busy, nothing copied or rolled back) OK\n";
    }

    // 6. Two threads meeting the same hot journal: one recovers it, the
    //    other waits and finds it spent. One copy, no error.
    for (int round = 0; round < 20; ++round) {
        const fs::path dir = root / ("race-" + std::to_string(round));
        fs::create_directories(dir);
        const fs::path crashed = makeInterrupted(dir);
        std::atomic<int> ready{0};
        seabass::infrastructure::PendingJournalRecovery results[2];
        const auto recover = [&](int i) {
            ++ready;
            while (ready < 2) {
            }
            results[i] = recoverPendingJournal(crashed, dir / "recovered",
                                               [&crashed]() { return openReadOnlyAndRead(crashed); },
                                               [&crashed]() { return openReadWriteAndRead(crashed); });
        };
        std::thread a(recover, 0);
        std::thread b(recover, 1);
        a.join();
        b.join();
        const int found = int(results[0].found) + int(results[1].found);
        const int recovered = int(results[0].recovered) + int(results[1].recovered);
        if (found != 1 || recovered != 1 || !results[0].error.empty() || !results[1].error.empty()) {
            std::cerr << "round " << round << ": found " << found << ", recovered " << recovered << ", errors '"
                      << results[0].error << "' '" << results[1].error << "'\n";
        }
        assert(found == 1 && recovered == 1 && "exactly one recovery");
        assert(results[0].error.empty() && results[1].error.empty());
        assert(readOnlySum(crashed) == 2000);
    }
    std::cout << "case 6 (two threads, one recovery, no error) OK\n";

    fs::remove_all(root);
    std::cout << "sqlite_pending_journal_test: all passed\n";
    return 0;
}
