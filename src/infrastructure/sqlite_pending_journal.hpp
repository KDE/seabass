// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <array>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::infrastructure
{

// A stick pulled in the middle of a save leaves a SQLite database with its
// rollback journal beside it: "<db>-journal", header intact. SQLite calls
// it hot, and the next connection that may write rolls the unfinished
// transaction back -- the database returns to where it was before that
// transaction, which is the whole point of the journal.
//
// Seabass reads every catalog read-only, and a read-only connection cannot
// roll anything back: it refuses to read at all, with "attempt to write a
// readonly database" at the first statement. Found in the macOS round 8
// manual check P3 (issue #48): after a pull mid-sync, every page that read
// OneLibrary failed with "failed to prepare statement ... readonly", the
// Sync page included, and with it the Undo that would have repaired it.

// The first eight bytes of a rollback journal with a live header. A journal
// SQLite has finished with is deleted, truncated or has this zeroed, and is
// not hot.
inline bool hasPendingJournal(const std::filesystem::path &database)
{
    std::filesystem::path journal = database;
    journal += "-journal";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(journal, ec) || std::filesystem::file_size(journal, ec) < 8 || ec) {
        return false;
    }
    std::ifstream in(journal, std::ios::binary);
    std::array<unsigned char, 8> header{};
    if (!in.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size()))) {
        return false;
    }
    static constexpr std::array<unsigned char, 8> magic{0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7};
    return header == magic;
}

struct PendingJournalRecovery
{
    bool found = false;      // there was a hot journal, one a read-only read could not get past
    bool recovered = false;  // and it has been rolled back
    bool busy = false;       // another connection held the database throughout: not checked
    std::filesystem::path keptCopy;  // where the database and journal were copied first
    std::string error;
};

// How a read ended: SQLite's extended result code (sqlite3_extended_errcode,
// 0 when it read) and its message. Plain numbers, so that this header needs
// neither sqlite3.h nor SQLCipher's copy of it.
struct SqliteReadOutcome
{
    int code = 0;
    std::string message;
};

namespace pending_journal_codes
{
constexpr int ok = 0;
constexpr int busy = 5;                 // SQLITE_BUSY
constexpr int locked = 6;               // SQLITE_LOCKED
constexpr int readOnlyRollback = 776;   // SQLITE_READONLY_ROLLBACK
}  // namespace pending_journal_codes

inline bool isSqliteBusy(int code)
{
    const int primary = code & 0xff;
    return primary == pending_journal_codes::busy || primary == pending_journal_codes::locked;
}

// One recovery at a time per database in this process: a second thread
// waits, then finds the journal spent, rather than copying it again or
// meeting the first one's rollback as a lock.
inline std::mutex &pendingJournalLockFor(const std::filesystem::path &database)
{
    static std::mutex guard;
    static std::map<std::string, std::unique_ptr<std::mutex>> locks;
    std::error_code ec;
    std::filesystem::path key = std::filesystem::weakly_canonical(database, ec);
    if (ec || key.empty()) {
        key = database;
    }
    const std::lock_guard<std::mutex> lock(guard);
    std::unique_ptr<std::mutex> &slot = locks[pathToUtf8(key)];
    if (!slot) {
        slot = std::make_unique<std::mutex>();
    }
    return *slot;
}

// Rolls a pending journal back, keeping a copy of both files first.
//
// A journal with a live header is also what a writer mid-commit leaves
// (Engine DJ, the CLI, Seabass on another thread): SQLite gives it that
// header just before writing pages, holding EXCLUSIVE. So a read-only
// read that fails is not proof of a pull. Only SQLITE_READONLY_ROLLBACK is:
// SQLite's own verdict that the journal is hot and no connection holds
// the lock that would make it live. BUSY or LOCKED means a writer; the
// read is tried again every 25 ms for `busyWait`, and after that the
// database is left alone and reported `busy`. Any other failure is not a
// hot journal either and is only reported.
//
// `readReadWrite` opens the database for writing and reads from it (plain
// SQLite for Engine, SQLCipher with its key for OneLibrary): SQLite rolls
// the journal back on that first read. The copy goes to
// `safekeepingRoot` on this computer, never to the stick, which may be the
// thing that is failing.
inline PendingJournalRecovery recoverPendingJournal(const std::filesystem::path &database,
                                                    const std::filesystem::path &safekeepingRoot,
                                                    const std::function<SqliteReadOutcome()> &readReadOnly,
                                                    const std::function<SqliteReadOutcome()> &readReadWrite,
                                                    std::chrono::milliseconds busyWait = std::chrono::milliseconds(0))
{
    PendingJournalRecovery result;
    const std::lock_guard<std::mutex> serial(pendingJournalLockFor(database));
    if (!hasPendingJournal(database)) {
        return result;
    }
    const auto deadline = std::chrono::steady_clock::now() + busyWait;
    const auto readWhileBusy = [&deadline](const std::function<SqliteReadOutcome()> &read) {
        SqliteReadOutcome outcome = read();
        while (isSqliteBusy(outcome.code) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            outcome = read();
        }
        return outcome;
    };
    const SqliteReadOutcome probe = readWhileBusy(readReadOnly);
    if (probe.code == pending_journal_codes::ok) {
        return result;
    }
    if (isSqliteBusy(probe.code)) {
        result.busy = true;
        result.error = probe.message;
        return result;
    }
    if (probe.code != pending_journal_codes::readOnlyRollback) {
        result.error = probe.message;
        return result;
    }
    result.found = true;

    std::filesystem::path journal = database;
    journal += "-journal";
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream stamp;
    stamp << std::put_time(&local, "%Y%m%dT%H%M%S");
    const std::filesystem::path keep = safekeepingRoot / pathFromUtf8(stamp.str() + "-" + pathToUtf8(database.filename()));
    std::error_code ec;
    std::filesystem::create_directories(keep, ec);
    if (!ec) {
        std::filesystem::copy_file(database, keep / database.filename(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    if (!ec) {
        std::filesystem::copy_file(journal, keep / journal.filename(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    if (ec) {
        // No copy, no rollback: the rollback is the one step here that
        // changes the stick, and it is not taken without a way back.
        result.error = "could not keep a copy of " + pathToUtf8(database.filename()) + " and its journal in "
            + pathToUtf8(keep) + ": " + ec.message();
        return result;
    }
    result.keptCopy = keep;

    // Another process rolling the same journal back holds the lock; once
    // the journal is spent, its rollback is as good as ours.
    const SqliteReadOutcome rolledBack = readWhileBusy(readReadWrite);
    if (rolledBack.code != pending_journal_codes::ok && hasPendingJournal(database)) {
        result.error = rolledBack.message;
        return result;
    }
    // Recovered means readable the way every reader reads: the same
    // read-only read that failed above, tried again.
    const SqliteReadOutcome check = readWhileBusy(readReadOnly);
    if (check.code != pending_journal_codes::ok) {
        result.error = "still cannot be read after opening it for writing: " + check.message;
        return result;
    }
    result.recovered = true;
    return result;
}

}  // namespace seabass::infrastructure
