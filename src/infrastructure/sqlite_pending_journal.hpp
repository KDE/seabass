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
#include <sstream>
#include <string>
#include <system_error>

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
    std::filesystem::path keptCopy;  // where the database and journal were copied first
    std::string error;
};

// Rolls a pending journal back, keeping a copy of both files first.
//
// `openReadWriteAndRead` opens the database for writing and reads from it
// (plain SQLite for Engine, SQLCipher with its key for OneLibrary): SQLite
// itself decides whether the journal is hot and rolls it back on that first
// read. It checks the locks to decide, so a save still running in another
// connection is never rolled back from under it. The copy goes to
// `safekeepingRoot` on this computer, never to the stick, which may be the
// thing that is failing.
// `openReadOnlyAndRead` is tried first: a journal with a live header is
// also what another connection mid-transaction leaves while it works
// (Engine DJ on the desktop, a second Seabass), and a read-only read then
// simply succeeds -- SQLite sees the RESERVED lock and the journal is not
// hot. Nothing is copied or touched in that case. Only a read-only read
// that fails is the pull's leftover, and then the copy and the roll back.
inline PendingJournalRecovery recoverPendingJournal(const std::filesystem::path &database,
                                                    const std::filesystem::path &safekeepingRoot,
                                                    const std::function<void()> &openReadOnlyAndRead,
                                                    const std::function<void()> &openReadWriteAndRead)
{
    PendingJournalRecovery result;
    if (!hasPendingJournal(database)) {
        return result;
    }
    try {
        openReadOnlyAndRead();
        return result;  // readable as it is: busy, not hot
    } catch (const std::exception &) {
        // The read a hot journal refuses; recovered below.
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

    try {
        openReadWriteAndRead();
    } catch (const std::exception &e) {
        result.error = e.what();
        return result;
    }
    // Recovered means readable the way every reader reads: the same
    // read-only read that failed above, tried again.
    try {
        openReadOnlyAndRead();
    } catch (const std::exception &e) {
        result.error = "still cannot be read after opening it for writing: " + std::string(e.what());
        return result;
    }
    result.recovered = true;
    return result;
}

}  // namespace seabass::infrastructure
