// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
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

// How many read copies this process has made: each is a whole database
// copied, so a test counts them the way WorkCounters counts opens.
inline std::atomic<std::uint64_t> &readCopiesMade()
{
    static std::atomic<std::uint64_t> made{0};
    return made;
}

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

// Whether `db` is a plain SQLite file with a rollback journal: both
// versions 1. False for WAL mode and for a header that cannot be read as
// a plain one (encrypted, or no file).
inline bool isRollbackModeSqlite(const std::filesystem::path &db)
{
    std::ifstream in(db, std::ios::binary);
    char header[20] = {};
    if (!in.read(header, sizeof header) || std::string_view(header, 16) != std::string_view("SQLite format 3\0", 16)) {
        return false;
    }
    return header[18] == 1 && header[19] == 1;
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
        readCopiesMade().fetch_add(1, std::memory_order_relaxed);
        try {
            fs::create_directories(m_path.parent_path());
            // The -wal first, the database last: a checkpoint between the
            // two copies moves pages the copied -wal still holds into the
            // database, which reads the same; the other way round, a
            // checkpoint that restarts the -wal would lose them (#110).
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
            } else if (!isRollbackModeSqlite(db) && !std::ofstream(walCopy, std::ios::binary)) {
                // Not beside a database with a rollback journal (copied
                // for its pending journal, see readWritesBeside()), which
                // a -wal would put in WAL mode.
                throw std::runtime_error("could not make " + pathToUtf8(walCopy));
            }
            // A pending journal goes with it: without it the copy would be
            // the pages a save left half written. The copy's first
            // read-write connection rolls it back, in the copy.
            fs::path journal = db;
            journal += "-journal";
            if (fs::is_regular_file(journal, ec)) {
                fs::path journalCopy = m_path;
                journalCopy += "-journal";
                fs::copy_file(journal, journalCopy);
            }
            fs::copy_file(db, m_path);
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

// Whether a read-only connection to `db` would write beside it: a
// WAL-mode database (the first connection truncates and rebuilds the
// -shm, and makes a -wal when there is none), or any database with a -wal
// beside it, which SQLite opens in WAL mode whatever the header says. Also
// one with a -journal beside it: the first connection that can write
// rolls a hot journal back, and libdjinterop's load_database() opens
// read-write. Recovering one on the stick is the readers' own step
// (recoverEnginePendingJournals(), which keeps a copy first), never a
// side effect of a read. A database with a rollback journal and nothing
// beside it is read without a write (#110).
inline bool readWritesBeside(const std::filesystem::path &db)
{
    std::error_code ec;
    std::filesystem::path wal = db;
    wal += "-wal";
    std::filesystem::path journal = db;
    journal += "-journal";
    if (isWalModeSqlite(db) || std::filesystem::exists(wal, ec) || ec) {
        return true;
    }
    return std::filesystem::exists(journal, ec) || ec;
}

// Makes a fresh read copy of a plain SQLite database a database with a
// rollback journal and nothing beside it: the -wal folded in, a pending
// journal rolled back, in the copy (sqlite_settle_copy.cpp). A copy read
// by many is then never changed by one of them: libdjinterop opens it
// read-write, and the last read-write connection to a WAL-mode database
// checkpoints and deletes its -wal as it closes, under every other
// reader. Does nothing to an encrypted database (OneLibrary's), whose
// readers all open it read-only. Throws when it cannot be done.
void settleReadCopy(const std::filesystem::path &copy);

// What a database on the stick and the files beside it look like, for
// telling whether a copy of it is still the database. Every commit to a
// WAL-mode database grows its -wal or, once a checkpoint has emptied it,
// restarts it under new salts and a new checkpoint sequence in its first
// 32 bytes; a checkpoint changes the database's own pages, and with them
// its header (the first 100 bytes, the file change counter at 24 among
// them); a save in rollback mode leaves or takes a -journal. Sizes and
// times are compared too, but are not relied on: FAT keeps a time to two
// seconds.
struct StickDatabaseStamp
{
    struct File
    {
        bool present = false;
        std::uintmax_t size = 0;
        std::filesystem::file_time_type modified{};
        std::string head;

        bool operator==(const File &) const = default;
    };
    File db;
    File wal;
    File journal;

    bool operator==(const StickDatabaseStamp &) const = default;

    static File fileStamp(const std::filesystem::path &file, std::size_t headBytes)
    {
        File out;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            return out;
        }
        out.present = true;
        out.size = std::filesystem::file_size(file, ec);
        out.modified = std::filesystem::last_write_time(file, ec);
        std::ifstream in(file, std::ios::binary);
        std::string head(headBytes, '\0');
        in.read(head.data(), static_cast<std::streamsize>(headBytes));
        head.resize(static_cast<std::size_t>(in.gcount()));
        out.head = std::move(head);
        return out;
    }

    static StickDatabaseStamp of(const std::filesystem::path &db)
    {
        std::filesystem::path wal = db;
        wal += "-wal";
        std::filesystem::path journal = db;
        journal += "-journal";
        return {fileStamp(db, 100), fileStamp(wal, 32), fileStamp(journal, 32)};
    }
};

// The read copies of the stick's databases: one per database, shared by
// the reads that only read, until the database changes (#110). A scan's
// count, catalog, cue and cover stages, Library Health's checks and a
// Browse page's rows read one copy between them, not one each.
//
// Every read compares the stick's files with the stamp the copy was taken
// at (StickDatabaseStamp) and takes a new copy when they differ: a copy is
// never older than the last commit to the stick, whoever made it, during
// one of Seabass's own saves too. forget() drops them all; the app calls
// it when a save ends and when the sticks change, so none outlives them.
// A save's own reads of what it writes never come here (ReadFor::Writing
// in StickDatabaseRead): they read the live file.
//
// A copy is taken -wal first and database last (DatabaseReadCopy), so a
// checkpoint between the two leaves pages in both, which reads the same,
// and is compared with the stamp taken before it: a commit while it was
// taken changes the stamp, and it is taken again. It is taken outside the
// lock: a read that only asks whether one is ready (mayCopy false, a row
// on the GUI thread), forget() and setWriting() never wait for a copy.
// Two reads that need the same copy share the one being made.
class StickReadSnapshots
{
public:
    static StickReadSnapshots &instance()
    {
        static StickReadSnapshots snapshots;
        return snapshots;
    }

    // The copy of `db` to read, `relative` its path below its catalog
    // folder. Made when there is none still good, unless `mayCopy` is
    // false: then nullptr, at once. Throws when it cannot be made.
    std::shared_ptr<const DatabaseReadCopy> snapshotOf(const std::filesystem::path &db,
                                                       const std::filesystem::path &relative, bool mayCopy = true)
    {
        const std::string key = pathToUtf8(db.lexically_normal()) + '\x1f' + pathToUtf8(relative);
        StickDatabaseStamp before = StickDatabaseStamp::of(db);
        std::uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            for (;;) {
                const auto found = m_snapshots.find(key);
                if (found != m_snapshots.end() && found->second.stamp == before) {
                    return found->second.copy;
                }
                if (!mayCopy) {
                    return nullptr;
                }
                if (m_copying.count(key) == 0) {
                    break;
                }
                // Another read is taking this copy: it is likely the one
                // wanted, so wait for it rather than take a second.
                m_copied.wait(lock);
                before = StickDatabaseStamp::of(db);
            }
            m_copying.insert(key);
            generation = m_generation;
        }
        const auto done = [this, &key]() {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_copying.erase(key);
            m_copied.notify_all();
        };
        try {
            constexpr int Attempts = 5;
            for (int attempt = 0; attempt < Attempts; ++attempt) {
                auto copy = std::make_shared<const DatabaseReadCopy>(db, relative);
                if (copyHookForTesting()) {
                    copyHookForTesting()();
                }
                const StickDatabaseStamp after = StickDatabaseStamp::of(db);
                if (after == before) {
                    settleReadCopy(copy->path());
                    {
                        const std::lock_guard<std::mutex> lock(m_mutex);
                        // Not kept past a forget() made while it was taken.
                        if (generation == m_generation) {
                            m_snapshots[key] = Snapshot{copy, after};
                        }
                    }
                    done();
                    return copy;
                }
                before = after;
            }
            throw std::runtime_error("it kept changing while it was copied");
        } catch (...) {
            done();
            throw;
        }
    }

    // Every copy dropped (a read still holding one keeps it until done).
    void forget()
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshots.clear();
        ++m_generation;
    }

    // Whether Seabass is writing a stick now. Reads compare stamps all the
    // same; the end of a write drops every copy, so the next read starts
    // from what the write left.
    void setWriting(bool writing)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        if (m_writing && !writing) {
            m_snapshots.clear();
            ++m_generation;
        }
        m_writing = writing;
    }

    // Called between taking a copy and checking it, outside the lock: a
    // test makes a copy slow with it.
    static std::function<void()> &copyHookForTesting()
    {
        static std::function<void()> hook;
        return hook;
    }

private:
    struct Snapshot
    {
        std::shared_ptr<const DatabaseReadCopy> copy;
        StickDatabaseStamp stamp;
    };
    std::mutex m_mutex;
    std::condition_variable m_copied;
    std::map<std::string, Snapshot> m_snapshots;
    std::set<std::string> m_copying;
    std::uint64_t m_generation = 0;
    bool m_writing = false;
};

// What a read of a stick database is for: only reading, or a lookup a
// save makes of the library it is writing (the stick itself or its
// scratch copy), which must see what that save has just written.
enum class ReadFor {
    Reading,
    Writing,
};

// A database on the stick as one read opens it (#110): reading a stick
// does not change it. The stick's own file where a read-only open writes
// nothing beside it, else the read copy StickReadSnapshots shares, held
// for as long as this object lives. For a save's own lookups
// (ReadFor::Writing) always the live file, never a copy: the save writes
// it anyway.
//
// `root()` stands for the catalog folder (Engine Library, PIONEER), so a
// caller that opens the database by its folder (libdjinterop's
// load_database()) opens the copy by the copy's; the catalog's other
// files, audio and analysis files, are still found from the real folder.
// Where there is no plain database to copy, the real one is named and the
// open fails as it always did. An always-copied one that is not there
// throws, as does a copy that cannot be made, saying why.
class StickDatabaseRead
{
public:
    enum class Copy {
        WhenItWouldWrite,  // a plain SQLite file: readWritesBeside() decides
        Always,  // an encrypted one (OneLibrary's), whose header cannot say
    };
    // What a read that needs a copy does when none is ready: makes one, or
    // goes without (a row asking on the GUI thread), ready() false.
    enum class IfNoCopy {
        Make,
        GoWithout,
    };

    StickDatabaseRead(const std::filesystem::path &catalogRoot, const std::filesystem::path &relative,
                      Copy when = Copy::WhenItWouldWrite, IfNoCopy ifNone = IfNoCopy::Make,
                      ReadFor purpose = ReadFor::Reading)
        : m_root(catalogRoot)
        , m_file(catalogRoot / relative)
    {
        if (purpose == ReadFor::Writing) {
            return;
        }
        // Copied even when it is not there, so the copy throws: a caller
        // that opens the database read-write would make an empty one on
        // the stick in its place.
        std::error_code ec;
        if (when != Copy::Always && !(std::filesystem::is_regular_file(m_file, ec) && readWritesBeside(m_file))) {
            return;
        }
        try {
            m_copy = StickReadSnapshots::instance().snapshotOf(m_file, relative, ifNone == IfNoCopy::Make);
        } catch (const std::exception &e) {
            std::error_code tempEc;
            throw std::runtime_error("could not copy " + pathToUtf8(m_file)
                                     + " to read it without changing the stick (is "
                                     + pathToUtf8(std::filesystem::temp_directory_path(tempEc))
                                     + " full?): " + e.what());
        }
        if (!m_copy) {
            m_ready = false;
            return;
        }
        m_root = m_copy->root();
        m_file = m_copy->path();
    }

    // Engine's m.db below its Engine Library folder.
    static StickDatabaseRead engine(const std::string &engineLibraryPath, IfNoCopy ifNone = IfNoCopy::Make,
                                    ReadFor purpose = ReadFor::Reading)
    {
        return StickDatabaseRead(pathFromUtf8(engineLibraryPath), std::filesystem::path("Database2") / "m.db",
                                 Copy::WhenItWouldWrite, ifNone, purpose);
    }

    // A database file named by itself, its folder standing for the catalog.
    static StickDatabaseRead file(const std::filesystem::path &db)
    {
        return StickDatabaseRead(db.parent_path(), db.filename());
    }

    const std::filesystem::path &root() const { return m_root; }
    const std::filesystem::path &path() const { return m_file; }
    std::string rootUtf8() const { return pathToUtf8(m_root); }
    std::string pathUtf8() const { return pathToUtf8(m_file); }
    bool copied() const { return m_copy != nullptr; }
    // False only when IfNoCopy::GoWithout found no copy ready: nothing to read.
    bool ready() const { return m_ready; }

private:
    std::shared_ptr<const DatabaseReadCopy> m_copy;
    std::filesystem::path m_root;
    std::filesystem::path m_file;
    bool m_ready = true;
};

}  // namespace seabass::infrastructure

