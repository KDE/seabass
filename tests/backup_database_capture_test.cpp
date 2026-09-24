// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <sqlite3.h>

#include <atomic>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "infrastructure/stick_backup/archive_updater.hpp"
#include "infrastructure/stick_backup/in_memory_archive_file.hpp"
#include "infrastructure/stick_backup/sqlite_db_set.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"

#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using namespace seabass::infrastructure::stick_backup;
namespace fs = std::filesystem;

namespace
{

void execSql(sqlite3 *db, const char *sql)
{
    char *error = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::cerr << "sqlite: " << (error ? error : "?") << " for: " << sql << "\n";
        sqlite3_free(error);
        assert(false);
    }
}

sqlite3 *openDb(const fs::path &path)
{
    sqlite3 *db = nullptr;
    assert(sqlite3_open(seabass::pathToUtf8(path).c_str(), &db) == SQLITE_OK);
    return db;
}

// A database with `rows` 8 KiB rows, so reads take long enough for a
// concurrent writer to land in the window.
void createDatabase(const fs::path &path, int rows)
{
    sqlite3 *db = openDb(path);
    execSql(db, "PRAGMA journal_mode=DELETE");
    execSql(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER, blob BLOB)");
    execSql(db, "BEGIN");
    for (int i = 0; i < rows; ++i) {
        execSql(db, "INSERT INTO t(v, blob) VALUES(0, zeroblob(8192))");
    }
    execSql(db, "COMMIT");
    sqlite3_close(db);
}

std::string readFile(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Harness
{
    std::shared_ptr<FaultClock> clock = std::make_shared<FaultClock>();
    InMemoryArchiveFile archive{clock};
    InMemoryArchiveFile journal{clock};
    std::unique_ptr<ArchiveUpdater> updater;
    Harness() : updater(std::make_unique<ArchiveUpdater>(archive, journal, std::vector<CentralEntry>{}, 0, 64 * 1024))
    {
        updater->begin();
    }
};

}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_backup_database_capture_test";
    fs::remove_all(root);
    fs::create_directories(root / "Engine Library" / "Database2");
    fs::path mainDb = root / "Engine Library" / "Database2" / "m.db";
    const std::string relativeMain = "Engine Library/Database2/m.db";

    // ---- Fingerprint: SQLite only, stable at rest, moves on commit ----
    {
        createDatabase(mainDb, 4);
        auto a = fingerprintDbSet(mainDb);
        auto b = fingerprintDbSet(mainDb);
        assert(a && b && *a == *b);
        assert(!a->hasWal && !a->hasJournal);
        sqlite3 *db = openDb(mainDb);
        execSql(db, "UPDATE t SET v = v + 1 WHERE id = 1");
        sqlite3_close(db);
        auto c = fingerprintDbSet(mainDb);
        assert(c && c->changeCounter == a->changeCounter + 1 && !(*c == *a));
        assert(c->toHex() != a->toHex() && c->toHex().size() == a->toHex().size());

        std::ofstream(root / "not-sqlite.db") << "just some bytes";
        assert(!fingerprintDbSet(root / "not-sqlite.db").has_value());
        assert(!fingerprintDbSet(root / "missing.db").has_value());
        std::cout << "case 1 (fingerprint stable at rest, moves on commit, non-SQLite rejected) OK\n";
    }

    // ---- Rollback journal mode with a hot journal: the set is two files ----
    {
        sqlite3 *db = openDb(mainDb);
        execSql(db, "BEGIN IMMEDIATE");
        execSql(db, "UPDATE t SET v = v + 100 WHERE id = 2");  // journal now on disk, transaction open
        fs::path journalFile = mainDb;
        journalFile += "-journal";
        assert(fs::exists(journalFile));
        assert(dbSetMembers(mainDb).size() == 2);
        auto fp = fingerprintDbSet(mainDb);
        assert(fp && fp->hasJournal && fp->journalSize > 0);

        Harness h;
        DbSetCapture capture = captureDbSet(root, relativeMain, *h.updater, 3);
        assert(capture.status == DbSetCapture::Status::Captured);
        assert(capture.entries.size() == 2);
        assert(capture.memberRelativePaths[0] == relativeMain);
        assert(capture.memberRelativePaths[1] == relativeMain + "-journal");
        assert(capture.fingerprint == *fp);
        assert(h.updater->newEntryCount() == 2);
        execSql(db, "ROLLBACK");
        sqlite3_close(db);
        assert(!fs::exists(journalFile));
        std::cout << "case 2 (hot rollback journal captured together with the main file) OK\n";
    }

    // ---- WAL mode: uncheckpointed frames live in -wal and are part of the set ----
    {
        fs::path walDb = root / "Engine Library" / "Database2" / "hm.db";
        createDatabase(walDb, 2);
        sqlite3 *db = openDb(walDb);
        execSql(db, "PRAGMA journal_mode=WAL");
        execSql(db, "UPDATE t SET v = 7 WHERE id = 1");  // sits in the WAL while the connection is open
        fs::path wal = walDb;
        wal += "-wal";
        assert(fs::exists(wal) && fs::file_size(wal) > 0);
        auto before = fingerprintDbSet(walDb);
        assert(before && before->hasWal && before->walSize > 0);

        Harness h;
        DbSetCapture capture = captureDbSet(root, "Engine Library/Database2/hm.db", *h.updater, 3);
        assert(capture.status == DbSetCapture::Status::Captured);
        assert(capture.entries.size() == 2);
        assert(capture.memberRelativePaths[1] == "Engine Library/Database2/hm.db-wal");
        // -shm exists on disk but is never a member.
        fs::path shm = walDb;
        shm += "-shm";
        assert(fs::exists(shm));
        assert(dbSetMembers(walDb).size() == 2);

        execSql(db, "UPDATE t SET v = 8 WHERE id = 2");  // WAL grows
        auto after = fingerprintDbSet(walDb);
        assert(after && after->walSize > before->walSize);
        sqlite3_close(db);
        std::cout << "case 3 (WAL captured with the main file, -shm ignored, WAL growth changes the fingerprint) OK\n";
    }

    // ---- A concurrent writer is detected; capture refuses to store a torn set ----
    {
        fs::path busyDb = root / "Engine Library" / "Database2" / "busy.db";
        createDatabase(busyDb, 1500);  // ~12 MiB: a read takes long enough to be interrupted
        std::atomic<bool> stop{false};
        std::thread writer([&] {
            sqlite3 *db = openDb(busyDb);
            execSql(db, "PRAGMA synchronous=OFF");
            while (!stop.load()) {
                execSql(db, "UPDATE t SET v = v + 1 WHERE id = 1");
            }
            sqlite3_close(db);
        });
        Harness h;
        DbSetCapture capture = captureDbSet(root, "Engine Library/Database2/busy.db", *h.updater, 2);
        stop.store(true);
        writer.join();
        assert(capture.status == DbSetCapture::Status::Unstable);
        assert(capture.entries.empty());
        assert(h.updater->newEntryCount() == 0);  // every torn attempt was forgotten
        assert(h.archive.size() > 0);              // ...but its bytes are dead space, as designed
        assert(capture.bytesRead > 0);

        // Writer gone: the same set captures cleanly and byte-exactly.
        Harness quiet;
        DbSetCapture clean = captureDbSet(root, "Engine Library/Database2/busy.db", *quiet.updater, 2);
        assert(clean.status == DbSetCapture::Status::Captured);
        assert(clean.entries.size() == 1);
        BackupManifest manifest;
        manifest.createdAtUnix = 1'757'000'000;
        manifest.rows.push_back({ManifestRow::Kind::File, "Engine Library/Database2/busy.db", clean.entries[0].entry.size,
                                 clean.memberMtimes[0], clean.entries[0].sha256, clean.fingerprint.toHex(),
                                 clean.entries[0].entry.crc32});
        quiet.updater->commit(manifest);
        Zip64Reader reader = Zip64Reader::open(quiet.archive);
        assert(reader.readEntryToString(0) == readFile(busyDb));
        std::cout << "case 4 (concurrent writer -> Unstable after retries, nothing listed; quiet -> byte-exact capture) OK\n";
    }

    // ---- A sidecar whose presence cannot be determined ----------------
    //
    // dbSetMembers() decided a -wal was absent from is_regular_file()
    // returning false, which it also does when the stat FAILED. A set
    // enumerated without its WAL is captured as a main file alone,
    // fingerprinted hasWal=false, and the second pass agrees with the
    // first about a set neither of them saw whole -- Captured, no
    // warning. The tree walk may have copied that WAL separately, and
    // restoring the pair hands SQLite a WAL whose salts do not match
    // the database.
    //
    // "Not there" must still be an answer, though: most databases have
    // no -wal, and refusing on any error would refuse nearly every
    // capture. So the case asserts both halves.
#if !defined(_WIN32)
    {
        fs::path db = root / "Engine Library" / "Database2" / "unknowable-wal.db";
        createDatabase(db, 20);

        // No sidecars at all: ordinary, and must still capture.
        {
            Harness plain;
            DbSetCapture ok = captureDbSet(root, "Engine Library/Database2/unknowable-wal.db", *plain.updater, 1);
            assert(ok.status == DbSetCapture::Status::Captured && "a database with no -wal is the common case");
        }

        // A -wal that is a symlink loop: present or absent cannot be
        // told, and the error is not "no such file".
        fs::path wal = db;
        wal += "-wal";
        fs::create_symlink(wal.filename(), wal);
        std::error_code loopEc;
        const bool unknowable = !fs::is_regular_file(wal, loopEc) && loopEc
                                && loopEc != std::errc::no_such_file_or_directory;
        assert(unknowable && "the sidecar's presence must really be undecidable for this case to mean anything");

        Harness h;
        DbSetCapture capture = captureDbSet(root, "Engine Library/Database2/unknowable-wal.db", *h.updater, 1);
        assert(capture.status != DbSetCapture::Status::Captured
               && "a set whose membership is unknown must not be captured as whole");
        assert(capture.status == DbSetCapture::Status::ReadError);
        assert(capture.entries.empty());
        assert(!capture.detail.empty());
        fs::remove(wal);
        std::cout << "case 4d (a sidecar whose presence cannot be determined refuses the capture) OK\n";
    }
#endif

    // ---- The same set, salvaged off a stick that has already failed ----
    //
    // Refusing an inconsistent set is right for a healthy stick: it means
    // something IS writing, and half a transaction is a database that
    // will not open, and the next run can have a clean one. Off a stick
    // the kernel has remounted read-only there is no writer and no next
    // run, so "kept changing while being read" means the device is
    // handing back different bytes -- and an Engine database that
    // probably opens beats none at all. It is the cues, the playlists
    // and the edits, which is most of what anyone wants the stick back
    // for.
    //
    // The writer thread stands in for that, because it produces the one
    // thing that matters here: a set whose members do not agree.
    {
        fs::path busyDb = root / "Engine Library" / "Database2" / "salvage.db";
        createDatabase(busyDb, 1500);
        std::atomic<bool> stop{false};
        std::thread writer([&] {
            sqlite3 *db = openDb(busyDb);
            execSql(db, "PRAGMA synchronous=OFF");
            while (!stop.load()) {
                execSql(db, "UPDATE t SET v = v + 1 WHERE id = 1");
            }
            sqlite3_close(db);
        });
        Harness h;
        DbSetCapture capture = captureDbSet(root, "Engine Library/Database2/salvage.db", *h.updater, 2, {},
                                            /*salvage=*/true);
        stop.store(true);
        writer.join();

        assert(capture.status == DbSetCapture::Status::Salvaged);
        assert(!capture.entries.empty() && "the attempt is kept, not thrown away");
        assert(capture.entries.size() == capture.memberRelativePaths.size());
        assert(capture.entries.size() == capture.memberMtimes.size());
        assert(h.updater->newEntryCount() == static_cast<int>(capture.entries.size())
               && "and the entries it lists are really in the archive");
        assert(!capture.detail.empty() && "with what happened said out loud");
        assert(capture.detail.find("may not agree") != std::string::npos);

        // Only on the last attempt: an intermittent fault is still worth
        // retrying, so the earlier passes are discarded as before. With
        // two retries that is three passes, and only the third is kept.
        std::cout << "case 4b (a salvage run keeps the database set it could not read cleanly) OK\n";
    }

    // ---- A member the stick stops giving part-way ----
    //
    // Measured on a real damaged stick (issue #36, A1, FAT chains cut):
    // m.db read 131072 of 274432 bytes. Refusing the set there keeps
    // none of it, and the part is the only copy of the cues anybody is
    // going to get. A salvage run keeps it and marks it; a healthy run
    // still refuses, because there a short read is a fault the next run
    // can put right.
    {
        const fs::path db = root / "Engine Library" / "Database2" / "partial.db";
        createDatabase(db, 1500);
        const std::uint64_t whole = fs::file_size(db);
        assert(whole > 8192);
        const auto stopsAt4k = [](const std::string &path) -> std::optional<std::uint64_t> {
            if (path == "Engine Library/Database2/partial.db") {
                return std::uint64_t{4096};
            }
            return std::nullopt;
        };

        Harness h;
        DbSetCapture capture =
            captureDbSet(root, "Engine Library/Database2/partial.db", *h.updater, 1, {}, /*salvage=*/true, stopsAt4k);
        assert(capture.status == DbSetCapture::Status::Salvaged && "the part is kept, not refused");
        assert(capture.entries.size() == capture.memberSalvagedFromSizes.size());
        assert(capture.entries[0].entry.size == 4096);
        assert(capture.memberSalvagedFromSizes[0] == whole && "and marked with what the file was");
        assert(h.updater->newEntryCount() == static_cast<int>(capture.entries.size()));
        assert(capture.detail.find("4096 of " + std::to_string(whole)) != std::string::npos);

        Harness healthy;
        DbSetCapture refused =
            captureDbSet(root, "Engine Library/Database2/partial.db", *healthy.updater, 1, {}, /*salvage=*/false, stopsAt4k);
        assert(refused.status == DbSetCapture::Status::ReadError && "a healthy stick's short read is still a fault");
        assert(refused.entries.empty() && healthy.updater->newEntryCount() == 0);
        // Two members truncated: the detail names both, not the last.
        {
            const fs::path journal = fs::path(db).concat("-journal");
            {
                std::ofstream out(journal, std::ios::binary);
                out << std::string(20'000, 'j');
            }
            const auto bothStop = [](const std::string &path) -> std::optional<std::uint64_t> {
                if (path.rfind("Engine Library/Database2/partial.db", 0) == 0) {
                    return std::uint64_t{4096};
                }
                return std::nullopt;
            };
            Harness two;
            DbSetCapture both =
                captureDbSet(root, "Engine Library/Database2/partial.db", *two.updater, 1, {}, /*salvage=*/true, bothStop);
            assert(both.status == DbSetCapture::Status::Salvaged);
            assert(both.detail.find("only 4096 of " + std::to_string(whole)) != std::string::npos
                   && "the main file's truncation is still said");
            assert(both.detail.find("partial.db-journal: only 4096 of 20000") != std::string::npos);
            fs::remove(journal);
        }
        std::cout << "case 4c (a salvage run keeps the part of a database the stick still gives, and marks it) OK\n";
    }

    // ---- >= 1 GiB refused without reading ----
    {
        fs::path huge = root / "Engine Library" / "Database2" / "huge.db";
        {
            std::ofstream out(huge, std::ios::binary);
            out << "SQLite format 3";
            out.put('\0');
        }
        fs::resize_file(huge, (std::uint64_t{1} << 30) + 1);  // sparse on every filesystem that matters
        Harness h;
        auto started = std::chrono::steady_clock::now();
        DbSetCapture capture = captureDbSet(root, "Engine Library/Database2/huge.db", *h.updater, 3);
        auto elapsed = std::chrono::steady_clock::now() - started;
        assert(capture.status == DbSetCapture::Status::TooLarge);
        assert(capture.bytesRead == 0 && h.archive.size() == 0);
        assert(elapsed < std::chrono::seconds(2));
        assert(capture.detail.find("huge.db") != std::string::npos);
        std::cout << "case 5 (>= 1 GiB database refused up front) OK\n";
    }

    fs::remove_all(root);
    std::cout << "all cases passed\n";
    return 0;
}
