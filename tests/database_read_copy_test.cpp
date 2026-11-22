// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// DatabaseReadCopy: a WAL-mode database read from a copy, read-only,
// without touching the original (#105).
//
// Case 1 is the one macOS's own SQLite fails without the empty -wal the
// copy makes (track_field_edit_change_test case 18 found it): a WAL-mode
// database whose -wal is gone, as SQLite leaves one after the last
// connection closes, cannot be opened read-only by that library unless a
// -wal exists beside it.

#include <sqlite3.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

#include "infrastructure/database_read_copy.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::pathToUtf8;
using seabass::infrastructure::DatabaseReadCopy;
using seabass::infrastructure::isWalModeSqlite;

namespace
{

void exec(sqlite3 *db, const char *sql)
{
    char *message = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
        std::cerr << sql << ": " << (message ? message : "") << "\n";
        sqlite3_free(message);
        assert(false && "the statement ran");
    }
}

sqlite3 *open(const fs::path &db, int flags)
{
    sqlite3 *handle = nullptr;
    const int rc = sqlite3_open_v2(pathToUtf8(db).c_str(), &handle, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "open " << pathToUtf8(db) << ": " << (handle ? sqlite3_errmsg(handle) : "out of memory") << "\n";
        sqlite3_close(handle);
        assert(false && "the database opens");
    }
    return handle;
}

// The rows a read-only connection sees, or -1 with the error printed:
// the first statement is where macOS's SQLite refuses a WAL-mode
// database without its -wal, after a successful open.
int trackCountReadOnly(const fs::path &db)
{
    sqlite3 *handle = open(db, SQLITE_OPEN_READONLY);
    sqlite3_stmt *stmt = nullptr;
    int count = -1;
    if (sqlite3_prepare_v2(handle, "SELECT count(*) FROM Track", -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "prepare on " << pathToUtf8(db) << ": " << sqlite3_errmsg(handle) << "\n";
    } else if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    } else {
        std::cerr << "step on " << pathToUtf8(db) << ": " << sqlite3_errmsg(handle) << "\n";
    }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);
    return count;
}

// Every file in the folder with its size and modification time.
std::map<std::string, std::pair<std::uintmax_t, fs::file_time_type>> listing(const fs::path &folder)
{
    std::map<std::string, std::pair<std::uintmax_t, fs::file_time_type>> files;
    for (const auto &entry : fs::recursive_directory_iterator(folder)) {
        if (entry.is_regular_file()) {
            files[pathToUtf8(entry.path())] = {entry.file_size(), entry.last_write_time()};
        }
    }
    return files;
}

// How many read-copy folders this process owns in the temporary folder.
int ownCopyFolders()
{
    const std::string prefix = std::string(seabass::infrastructure::ReadCopyPrefix)
        + seabass::infrastructure::readCopyOwnerTag();
    int count = 0;
    for (const auto &entry : fs::directory_iterator(fs::temp_directory_path())) {
        if (pathToUtf8(entry.path().filename()).rfind(prefix, 0) == 0) {
            ++count;
        }
    }
    return count;
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "database_read_copy";
    fs::remove_all(root);
    const fs::path database2 = root / "Engine Library" / "Database2";
    fs::create_directories(database2);
    const fs::path db = database2 / "m.db";
    const fs::path wal = database2 / "m.db-wal";
    const fs::path shm = database2 / "m.db-shm";
    {
        sqlite3 *handle = open(db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        exec(handle, "CREATE TABLE Track (id INTEGER PRIMARY KEY, path TEXT)");
        exec(handle, "INSERT INTO Track (path) VALUES ('a.mp3'), ('b.mp3'), ('c.mp3')");
        exec(handle, "PRAGMA journal_mode=WAL");
        sqlite3_close(handle);
    }
    // The switch to WAL mode opens no WAL itself, so a connection closed
    // straight after it leaves no -wal or -shm with any SQLite (measured:
    // macOS's 3.51, upstream 3.53). A connection that then reads or
    // writes opens both, and there the libraries part: upstream deletes
    // them when the last connection closes, macOS's keeps them (case 2
    // cleans up after its own). The case here is the database without
    // them, which is how a stick arrives from a writer that closed.
    assert(isWalModeSqlite(db) && "the database is in WAL mode");
    assert(!fs::exists(wal) && !fs::exists(shm) && "and has no -wal or -shm beside it");

    // Case 1: a WAL-mode database with no -wal is read whole from its
    // copy, read-only, and the original is not touched.
    {
        const auto before = listing(root);
        fs::path copyRoot;
        {
            const DatabaseReadCopy copy(db, fs::path("Database2") / "m.db");
            copyRoot = copy.root();
            assert(copy.path() == copyRoot / "Database2" / "m.db");
            assert(fs::is_regular_file(copy.path()) && "the database is copied");
            fs::path copyWal = copy.path();
            copyWal += "-wal";
            assert(fs::is_regular_file(copyWal) && fs::file_size(copyWal) == 0 && "with an empty -wal beside it");
            assert(trackCountReadOnly(copy.path()) == 3 && "the copy is read read-only, whole");
        }
        assert(!fs::exists(copyRoot) && "the copy goes with the object");
        assert(listing(root) == before && "nothing on the stick changed");
        assert(!fs::exists(wal) && !fs::exists(shm) && "the stick got no -wal or -shm");
        std::cout << "case 1 (a WAL-mode database without its -wal is read from a copy) OK\n";
    }

    // Case 2: a row only the -wal holds (a writer with checkpoints off
    // still has the database open) is in the copy too.
    {
        sqlite3 *held = open(db, SQLITE_OPEN_READWRITE);
        exec(held, "PRAGMA wal_autocheckpoint = 0");
        exec(held, "INSERT INTO Track (path) VALUES ('only-in-the-wal.mp3')");
        assert(fs::is_regular_file(wal) && fs::file_size(wal) > 0 && "the new row is in the -wal");
        {
            const DatabaseReadCopy copy(db, fs::path("Database2") / "m.db");
            fs::path copyWal = copy.path();
            copyWal += "-wal";
            assert(fs::file_size(copyWal) == fs::file_size(wal) && "the -wal is copied as it is");
            assert(trackCountReadOnly(copy.path()) == 4 && "the copy holds the row the -wal holds");
        }
        sqlite3_close(held);
        std::cout << "case 2 (a row only the -wal holds is in the copy) OK\n";
    }

    // Case 3: a -wal that cannot be looked at is not "no -wal". The copy
    // refuses rather than standing an empty one in for it, and leaves no
    // folder behind. A symlink loop is what makes the look fail without
    // touching permissions; making one needs a privilege on Windows, so
    // the case is POSIX only.
#if !defined(_WIN32)
    {
        fs::remove(wal);
        fs::remove(shm);
        fs::create_symlink("m.db-wal", wal);
        std::error_code ec;
        assert(!fs::is_regular_file(wal, ec) && ec && "the loop is a look that fails, not a missing file");
        const auto copiesBefore = ownCopyFolders();
        bool refused = false;
        try {
            const DatabaseReadCopy copy(db, fs::path("Database2") / "m.db");
        } catch (const std::runtime_error &e) {
            refused = std::string(e.what()).find("m.db-wal") != std::string::npos;
        }
        assert(refused && "the copy refuses, naming the -wal");
        assert(ownCopyFolders() == copiesBefore && "and leaves no folder behind");
        fs::remove(wal);
        std::cout << "case 3 (a -wal that cannot be looked at refuses the copy) OK\n";
    }
#endif

    fs::remove_all(root);
    return 0;
}
