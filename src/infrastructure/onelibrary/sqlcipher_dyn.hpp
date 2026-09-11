// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

// A minimal, self-contained SQLite/SQLCipher C-API binding loaded at
// runtime via LoadLibrary/GetProcAddress against libsqlcipher-0.dll,
// rather than linked at build time.
//
// Why: Seabass already links plain (unencrypted) SQLite3 elsewhere
// (LocalCueStore, libdjinterop's bundled amalgamation). SQLCipher is
// ABI-compatible with SQLite3 and exports the *same* symbol names
// (sqlite3_open_v2, sqlite3_exec, ...) from its own DLL. Statically
// linking both into one executable would leave the linker to arbitrarily
// pick one implementation for every call site sharing those symbol
// names -- silently breaking whichever caller needed the other one.
// Loading libsqlcipher-0.dll explicitly and calling through function
// pointers keeps the two entirely separate; nothing here ever touches a
// symbol also provided by plain sqlite3.
//
// Deliberately declares its own opaque types/constants instead of
// including sqlite3.h, for the same isolation reason -- this header
// must never accidentally pull in (or conflict with) the plain-SQLite3
// declarations used elsewhere in this codebase.

#include <cstdint>
#include <stdexcept>
#include <string>

namespace seabass::infrastructure::onelibrary
{

struct sqlite3;
struct sqlite3_stmt;

constexpr int SQLITE_OK = 0;
constexpr int SQLITE_ROW = 100;
constexpr int SQLITE_DONE = 101;
constexpr int SQLITE_OPEN_READWRITE = 0x00000002;
constexpr int SQLITE_OPEN_CREATE = 0x00000004;
constexpr int SQLITE_INTEGER = 1;
constexpr int SQLITE_NULL = 5;

// Loads libsqlcipher-0.dll (searched next to the running executable and
// on the system PATH) and resolves the handful of C-API entry points
// this module needs. Throws std::runtime_error if the DLL or any symbol
// can't be found. One process-wide instance is enough; SqlCipherDb below
// takes a reference to it rather than loading the library itself.
class SqlCipherLibrary
{
public:
    SqlCipherLibrary();
    ~SqlCipherLibrary();
    SqlCipherLibrary(const SqlCipherLibrary &) = delete;
    SqlCipherLibrary &operator=(const SqlCipherLibrary &) = delete;

    int open(const char *filename, sqlite3 **db, int flags) const;
    int close(sqlite3 *db) const;
    int exec(sqlite3 *db, const char *sql) const;
    int prepare(sqlite3 *db, const char *sql, sqlite3_stmt **stmt) const;
    int step(sqlite3_stmt *stmt) const;
    int finalize(sqlite3_stmt *stmt) const;
    int bindInt64(sqlite3_stmt *stmt, int index, int64_t value) const;
    int bindText(sqlite3_stmt *stmt, int index, const std::string &value) const;
    int bindNull(sqlite3_stmt *stmt, int index) const;
    int64_t columnInt64(sqlite3_stmt *stmt, int index) const;
    std::string columnText(sqlite3_stmt *stmt, int index) const;
    int columnType(sqlite3_stmt *stmt, int index) const;
    std::string errmsg(sqlite3 *db) const;

private:
    void *m_module = nullptr;
    struct Fns;
    Fns *m_fns = nullptr;
};

// RAII-wrapped connection, opened with the well-known, universal
// OneLibrary/Device Library Plus SQLCipher key (see onelibrary_key.hpp)
// applied via `PRAGMA key` immediately after opening -- SQLCipher 4's own
// compiled-in defaults (kdf_iter, page size, HMAC/KDF algorithm) are used
// as-is, matching what pyrekordbox's reference implementation does (no
// PRAGMA overrides at all) and confirmed by successfully decrypting a
// real exportLibrary.db copy with exactly this scheme during development.
class SqlCipherDb
{
public:
    SqlCipherDb(const SqlCipherLibrary &lib, const std::string &path, bool readOnly);
    ~SqlCipherDb();
    SqlCipherDb(const SqlCipherDb &) = delete;
    SqlCipherDb &operator=(const SqlCipherDb &) = delete;

    sqlite3 *handle() const { return m_db; }
    const SqlCipherLibrary &lib() const { return m_lib; }

    void exec(const std::string &sql) const;

private:
    const SqlCipherLibrary &m_lib;
    sqlite3 *m_db = nullptr;
};

// Thin prepared-statement RAII wrapper, mirroring PdbRowWriter/LocalCueStore's
// own Statement helper style in this codebase.
class SqlCipherStatement
{
public:
    SqlCipherStatement(const SqlCipherDb &db, const std::string &sql);
    ~SqlCipherStatement();
    SqlCipherStatement(const SqlCipherStatement &) = delete;
    SqlCipherStatement &operator=(const SqlCipherStatement &) = delete;

    void bindInt64(int index, int64_t value);
    void bindText(int index, const std::string &value);
    void bindNull(int index);
    // Runs to completion (for INSERT/UPDATE/DELETE); throws on failure.
    void run();
    // For SELECTs: advances to the next row, returns false once exhausted.
    bool step();
    int64_t columnInt64(int index);
    std::string columnText(int index);
    bool columnIsNull(int index);

private:
    const SqlCipherDb &m_db;
    sqlite3_stmt *m_stmt = nullptr;
};

}  // namespace seabass::infrastructure::onelibrary
