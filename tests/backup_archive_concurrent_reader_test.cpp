// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A background reader must not be able to damage a backup that is being
// written.
//
// The bug this pins down, from a real 12.3 GB archive that turned out to
// hold 3.8 MB of actual bytes: RestoreStickBackup's describe()/
// describeAll() -- pure listing calls, used by BackupAdvisorController on
// every stick assessment -- used to open an archive ReadWrite and run
// recoverOnOpen() on it. A backup that is *running* has a live journal by
// design, so a listing pass mid-backup read that journal as "an update
// never confirmed" and truncated the archive back, while BackupStick kept
// appending at its cached size. Writing past a shrunken file re-extends
// it sparsely: an archive of exactly the expected length containing
// nothing but its trailer.

#include <sqlite3.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include <atomic>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "application/use_cases/backup_stick.hpp"
#include "application/use_cases/restore_stick_backup.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"

#include "scratch_path.hpp"

using namespace seabass::application;
using namespace seabass::infrastructure::stick_backup;
namespace fs = std::filesystem;

namespace
{

std::string pseudoRandom(std::size_t size, std::uint64_t seed)
{
    std::string out(size, '\0');
    std::uint64_t x = seed * 0x9E3779B97F4A7C15ull + 1;
    for (std::size_t i = 0; i < size; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out[i] = static_cast<char>(x & 0xff);
    }
    return out;
}

void writeFile(const fs::path &p, const std::string &content, std::int64_t mtime)
{
    fs::create_directories(p.parent_path());
    {
        std::ofstream out(p, std::ios::binary);
        out << content;
    }
    fs::last_write_time(p, fromUnixSeconds(mtime));
}

void createEngineDb(const fs::path &path)
{
    fs::create_directories(path.parent_path());
    sqlite3 *db = nullptr;
    assert(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "CREATE TABLE Track(id INTEGER PRIMARY KEY, path TEXT); INSERT INTO Track(path) VALUES('a')",
                        nullptr, nullptr, nullptr)
           == SQLITE_OK);
    sqlite3_close(db);
}

// Bytes the filesystem actually allocated, as opposed to the length the
// file reports. A hollow archive is caught by this alone: the sparse one
// from the field reported 12.3 GB and had 3.8 MB behind it.
std::uint64_t allocatedBytes(const fs::path &p)
{
#if defined(_WIN32)
    // GetCompressedFileSize reports actual on-disk usage for both
    // compressed and sparse files (st_blocks's Windows equivalent --
    // struct stat here has no st_blocks at all).
    DWORD high = 0;
    DWORD low = GetCompressedFileSizeW(p.c_str(), &high);
    assert(low != INVALID_FILE_SIZE || GetLastError() == NO_ERROR);
    return (static_cast<std::uint64_t>(high) << 32) | low;
#else
    struct stat st{};
    assert(::stat(p.c_str(), &st) == 0);
    return static_cast<std::uint64_t>(st.st_blocks) * 512u;
#endif
}

void assertNotHollow(const fs::path &archive)
{
    const std::uint64_t apparent = fs::file_size(archive);
    const std::uint64_t allocated = allocatedBytes(archive);
    // Generous: entries are stored uncompressed, so allocation tracks
    // length closely. Half is far below anything a healthy archive
    // produces and far above a hollow one.
    if (allocated * 2 < apparent) {
        std::cerr << "archive is hollow: reports " << apparent << " bytes, only " << allocated << " allocated\n";
        assert(false && "archive is sparse -- data was never written");
    }
}

struct Fixture
{
    fs::path root;
    fs::path stick;
    fs::path archive;
    BackupStickOptions options;

    explicit Fixture(const std::string &name)
        : root(seabass::testing::scratchRoot() / ("seabass_concurrent_reader_test_" + name)), stick(root / "stick"),
          archive(root / "Seabass Backups" / "STICK.zip")
    {
        fs::remove_all(root);
        // Enough files that the reader below lands mid-run rather than
        // after everything is already written.
        for (int i = 0; i < 40; ++i) {
            writeFile(stick / "Contents" / ("track" + std::to_string(i) + ".mp3"), pseudoRandom(120'000, i + 1),
                      1'700'000'000 + i);
        }
        writeFile(stick / "PIONEER" / "rekordbox" / "export.pdb", pseudoRandom(4096, 99), 1'700'000'100);
        createEngineDb(stick / "Engine Library" / "Database2" / "m.db");
        options.stickRoot = stick;
        options.archivePath = archive;
        options.stickIdentifier = "uuid-stick";
        options.stickLabel = "STICK";
    }
    ~Fixture()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

}  // namespace

int main()
{
    // ---- describeAll() over the folder while a backup runs ----
    // Exactly what BackupAdvisorController does on every stick
    // assessment, app-wide, in the background.
    {
        Fixture f("describe-all-during-backup");
        const fs::path directory = f.archive.parent_path();
        std::atomic<int> passes{0};
        f.options.onProgress = [&](const BackupProgress &progress) {
            if (progress.phase != BackupProgress::Phase::Reading || progress.filesDone < 3 || passes >= 3) {
                return;
            }
            ++passes;
            // Another thread, like the advisor's own worker.
            std::thread reader([&] { (void)RestoreStickBackup::describeAll(directory); });
            reader.join();
        };
        BackupStickOutcome outcome = BackupStick::execute(f.options);
        assert(passes > 0 && "the reader never ran -- the test proves nothing");
        if (outcome.status != BackupOutcomeStatus::Complete) {
            std::cerr << "backup did not complete: " << outcome.message << "\n";
        }
        assert(outcome.status == BackupOutcomeStatus::Complete);

        VerifyOutcome verified = BackupStick::verify(f.archive);
        if (!verified.error.empty()) {
            std::cerr << "verify error: " << verified.error << "\n";
        }
        for (const std::string &failure : verified.failures) {
            std::cerr << "damaged entry: " << failure << "\n";
        }
        assert(verified.error.empty());
        assert(verified.ok);
        assertNotHollow(f.archive);
        std::cout << "case 1 (describeAll during a backup leaves a good archive) OK\n";
    }

    // ---- describe() on the archive being written ----
    {
        Fixture f("describe-one-during-backup");
        std::atomic<int> passes{0};
        f.options.onProgress = [&](const BackupProgress &progress) {
            if (progress.phase != BackupProgress::Phase::Reading || progress.filesDone < 3 || passes >= 3) {
                return;
            }
            ++passes;
            std::thread reader([&] { (void)RestoreStickBackup::describe(f.archive); });
            reader.join();
        };
        assert(BackupStick::execute(f.options).status == BackupOutcomeStatus::Complete);
        assert(passes > 0);
        VerifyOutcome verified = BackupStick::verify(f.archive);
        assert(verified.error.empty() && verified.ok);
        assertNotHollow(f.archive);
        std::cout << "case 2 (describe during a backup leaves a good archive) OK\n";
    }

    // ---- the same across a cancel/keep generation ----
    // The sequence that produced the archive from the field: cancel part
    // way, keep, run again to completion, then verify.
    {
        Fixture f("describe-during-resumed-backup");
        CancellationToken cancel;
        f.options.cancel = cancel;
        f.options.onProgress = [&](const BackupProgress &progress) {
            if (progress.phase == BackupProgress::Phase::Reading && progress.filesDone >= 5) {
                cancel.cancel();
            }
        };
        BackupStickOutcome first = BackupStick::execute(f.options);
        assert(first.status == BackupOutcomeStatus::Cancelled);
        assert(first.pending != nullptr);
        assert(first.pending->keep().status == BackupOutcomeStatus::KeptPartial);

        const fs::path directory = f.archive.parent_path();
        std::atomic<int> passes{0};
        BackupStickOptions resume = f.options;
        resume.cancel = CancellationToken::none();
        resume.onProgress = [&](const BackupProgress &progress) {
            if (progress.phase != BackupProgress::Phase::Reading || passes >= 3) {
                return;
            }
            ++passes;
            std::thread reader([&] { (void)RestoreStickBackup::describeAll(directory); });
            reader.join();
        };
        assert(BackupStick::execute(resume).status == BackupOutcomeStatus::Complete);
        VerifyOutcome verified = BackupStick::verify(f.archive);
        if (!verified.error.empty()) {
            std::cerr << "verify error after resume: " << verified.error << "\n";
        }
        assert(verified.error.empty());
        assert(verified.ok);
        assertNotHollow(f.archive);
        std::cout << "case 3 (cancel, keep, resume with a reader alongside) OK\n";
    }

    // ---- the backup side's own preview, alongside a run ----
    // StickBackupController::refresh() only guards against a second
    // preview, not against a run, so this pairing is reachable from the
    // UI; BackupStick::preview() used to recover (and so truncate) too.
    {
        Fixture f("preview-during-backup");
        std::atomic<int> passes{0};
        BackupStickOptions probe = f.options;
        probe.onProgress = {};
        f.options.onProgress = [&](const BackupProgress &progress) {
            if (progress.phase != BackupProgress::Phase::Reading || progress.filesDone < 3 || passes >= 3) {
                return;
            }
            ++passes;
            std::thread reader([&] { (void)BackupStick::preview(probe); });
            reader.join();
        };
        BackupStickOutcome outcome = BackupStick::execute(f.options);
        assert(passes > 0);
        if (outcome.status != BackupOutcomeStatus::Complete) {
            std::cerr << "backup did not complete: " << outcome.message << "\n";
        }
        assert(outcome.status == BackupOutcomeStatus::Complete);
        VerifyOutcome verified = BackupStick::verify(f.archive);
        assert(verified.error.empty() && verified.ok);
        assertNotHollow(f.archive);
        std::cout << "case 4 (preview during a backup leaves a good archive) OK\n";
    }

    std::cout << "All backup_archive_concurrent_reader tests passed." << std::endl;
    return 0;
}
