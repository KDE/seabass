// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/system/process_liveness.hpp"

namespace seabass::infrastructure
{

// A database on the stick read without touching the stick (#105).
//
// A WAL-mode SQLite database cannot be read read-only without writing its
// -shm: SQLite keeps the WAL's index there and updates it on every read
// (measured on a copy of the fixture: one lookup gave the stick's
// exportLibrary.db-shm a new modification time). Such a database is read
// from a copy of it and its -wal, made in the temporary folder
// (std::filesystem::temp_directory_path(), where FormatWriteSession puts
// its scratch copies too): the copy holds every committed row, and
// SQLite rebuilds the -shm next to the copy. The folder goes with the
// object.
//
// A copy always has a -wal beside it, an empty one when the stick has
// none: macOS's own SQLite (the one the Mac build links, 3.51) refuses a
// read-only connection to a WAL-mode database whose -wal does not exist,
// "unable to open database file" at the first statement, where upstream
// SQLite, so Linux and Windows, makes the -wal itself. A -wal with no
// frames in it reads as no change: the copy answers from its pages alone.
//
// Folders are named ReadCopyPrefix + the owning process's pid and start
// id + a serial. One left behind by a crash is removed by
// removeStaleReadCopies() once its owner is gone (isProcessAlive(), the
// edit locks' test), so a copy another running Seabass is reading is
// never taken from it, however long ago it was made. A folder whose name
// carries no owner (an older build's) goes by age instead: older than
// this process.

inline constexpr const char *ReadCopyPrefix = "seabass-catalog-read-";

// When this process first asked: every read copy it makes is newer.
inline std::filesystem::file_time_type readCopiesSince()
{
    static const auto since = std::filesystem::file_time_type::clock::now();
    return since;
}

// Whether `db`, a plain (unencrypted) SQLite file, is in WAL mode: the
// header's file format write and read versions (bytes 18 and 19) are 2
// for WAL, 1 for a rollback journal. An encrypted database (SQLCipher,
// OneLibrary's exportLibrary.db) has its header encrypted and cannot be
// told this way: its caller copies it always.
inline bool isWalModeSqlite(const std::filesystem::path &db)
{
    std::ifstream in(db, std::ios::binary);
    char header[20] = {};
    if (!in.read(header, sizeof header)) {
        return false;
    }
    return header[18] == 2 || header[19] == 2;
}

// "<pid>-<start id>-" after ReadCopyPrefix: this process's part of a
// read copy's folder name.
inline std::string readCopyOwnerTag()
{
    return std::to_string(system::currentPid()) + "-" + std::to_string(system::currentProcessStartId()) + "-";
}

// The owner a folder name carries, as {pid, start id}; false for a name
// that carries none (a folder an older build made).
inline bool readCopyOwner(const std::string &afterPrefix, std::int64_t &pid, std::uint64_t &startId)
{
    const auto firstDash = afterPrefix.find('-');
    const auto secondDash = firstDash == std::string::npos ? std::string::npos : afterPrefix.find('-', firstDash + 1);
    const auto thirdDash = secondDash == std::string::npos ? std::string::npos : afterPrefix.find('-', secondDash + 1);
    // Owner-tagged names have four parts (pid, start id, clock, serial);
    // an older build's had two (clock, serial).
    if (thirdDash == std::string::npos) {
        return false;
    }
    try {
        std::size_t used = 0;
        pid = std::stoll(afterPrefix.substr(0, firstDash), &used);
        if (used != firstDash) {
            return false;
        }
        startId = std::stoull(afterPrefix.substr(firstDash + 1, secondDash - firstDash - 1), &used);
        return used == secondDash - firstDash - 1;
    } catch (const std::exception &) {
        return false;
    }
}

inline void removeStaleReadCopies()
{
    namespace fs = std::filesystem;
    const auto since = readCopiesSince();
    const std::string prefix = ReadCopyPrefix;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(fs::temp_directory_path(ec), ec)) {
        const std::string name = pathToUtf8(entry.path().filename());
        if (name.rfind(prefix, 0) != 0 || !entry.is_directory(ec)) {
            continue;
        }
        std::int64_t pid = 0;
        std::uint64_t startId = 0;
        if (readCopyOwner(name.substr(prefix.size()), pid, startId)) {
            if (!system::isProcessAlive(pid, startId)) {
                fs::remove_all(entry.path(), ec);
            }
            continue;
        }
        std::error_code timeEc;
        const auto written = entry.last_write_time(timeEc);
        if (!timeEc && written < since) {
            fs::remove_all(entry.path(), ec);
        }
    }
}

class DatabaseReadCopy
{
public:
    // Copies `db` and its -wal, when there is one, to `relative` (the
    // database's path below its catalog folder, "Database2/m.db") inside
    // a fresh folder. Throws when the copy cannot be made.
    DatabaseReadCopy(const std::filesystem::path &db, const std::filesystem::path &relative)
    {
        namespace fs = std::filesystem;
        readCopiesSince();
        static std::atomic<unsigned> serial{0};
        m_root = fs::temp_directory_path()
            / (std::string(ReadCopyPrefix) + readCopyOwnerTag()
               + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-"
               + std::to_string(serial++));
        m_path = m_root / relative;
        try {
            fs::create_directories(m_path.parent_path());
            fs::copy_file(db, m_path);
            fs::path wal = db;
            wal += "-wal";
            fs::path walCopy = m_path;
            walCopy += "-wal";
            // Only a -wal that is not there is "no -wal". A look that
            // fails (status() reports not_found with `ec` set, so the type
            // is what tells), or something else under the name, is not:
            // an empty -wal in its place would lose the rows only the
            // stick's -wal holds.
            std::error_code ec;
            const fs::file_status walStatus = fs::status(wal, ec);
            if (walStatus.type() == fs::file_type::regular) {
                fs::copy_file(wal, walCopy);
            } else if (walStatus.type() != fs::file_type::not_found) {
                throw std::runtime_error("could not look for " + pathToUtf8(wal) + ": "
                                         + (ec ? ec.message() : "not a file"));
            } else if (!std::ofstream(walCopy, std::ios::binary)) {
                throw std::runtime_error("could not make " + pathToUtf8(walCopy));
            }
        } catch (...) {
            std::error_code ec;
            fs::remove_all(m_root, ec);
            throw;
        }
    }
    ~DatabaseReadCopy()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_root, ec);
    }
    DatabaseReadCopy(const DatabaseReadCopy &) = delete;
    DatabaseReadCopy &operator=(const DatabaseReadCopy &) = delete;

    // The folder the copy is in, standing for the catalog folder.
    const std::filesystem::path &root() const { return m_root; }
    const std::filesystem::path &path() const { return m_path; }

private:
    std::filesystem::path m_root;
    std::filesystem::path m_path;
};

}  // namespace seabass::infrastructure
