// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// An archive-level write lock so a restore, a clone (backup+restore) and
// a compaction cannot write the same archive a backup is still writing,
// regardless of thread, process or the CLI/GUI boundary -- named as
// still-open by the commits that fixed the *reading* side of this same
// race (see backup_archive_concurrent_reader_test.cpp): describe()/
// preview() no longer recover, so they no longer need this, but nothing
// stopped two writers landing on the same archive at once.
//
// BackupStick, RestoreStickBackup and CompactStickBackup each take a
// StickWriteLock on `<archive>.lock` for the whole call (BackupStick:
// for the whole PendingBackup session, since a cancelled run can await
// keep()/discard() long after execute() returns). A second writer gets
// refused immediately (StickBusyError, surfaced as a Failed/refused
// outcome with a message naming the conflict) rather than racing to
// open the file.

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "application/use_cases/backup_stick.hpp"
#include "application/use_cases/compact_stick_backup.hpp"
#include "application/use_cases/restore_stick_backup.hpp"

#include "scratch_path.hpp"

using namespace seabass::application;
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

void writeFile(const fs::path &p, const std::string &content)
{
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << content;
}

struct Fixture
{
    fs::path root;
    fs::path stick;
    fs::path archive;
    BackupStickOptions options;

    explicit Fixture(const std::string &name)
        : root(seabass::testing::scratchRoot() / ("seabass_archive_write_lock_test_" + name)), stick(root / "stick"),
          archive(root / "Seabass Backups" / "STICK.zip")
    {
        fs::remove_all(root);
        // Enough files that onProgress fires while the backup is still
        // mid-write, so the injected concurrent call lands in that window
        // rather than after everything is already committed.
        for (int i = 0; i < 30; ++i) {
            writeFile(stick / "Contents" / ("track" + std::to_string(i) + ".mp3"), pseudoRandom(120'000, i + 1));
        }
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

// Runs `f`'s backup to completion, invoking `duringWrite` once, from
// another thread, partway through -- the same synchronous-callback
// technique backup_archive_concurrent_reader_test.cpp uses, deterministic
// rather than timing-dependent.
BackupStickOutcome runBackupWithInjectedCall(Fixture &f, const std::function<void()> &duringWrite)
{
    std::atomic<bool> injected{false};
    f.options.onProgress = [&](const BackupProgress &progress) {
        if (progress.phase != BackupProgress::Phase::Reading || progress.filesDone < 3 || injected) {
            return;
        }
        injected = true;
        std::thread other(duringWrite);
        other.join();
    };
    BackupStickOutcome outcome = BackupStick::execute(f.options);
    assert(injected && "the injected call never ran -- the test proves nothing");
    return outcome;
}

}  // namespace

int main()
{
    // ---- a restore refuses while a backup is writing ----
    {
        Fixture f("restore-vs-backup");
        RestoreSummary restoreResult;
        BackupStickOutcome outcome = runBackupWithInjectedCall(f, [&] {
            RestoreOptions restoreOptions;
            restoreOptions.archivePath = f.archive;
            restoreOptions.targetRoot = f.root / "restore-target";
            fs::create_directories(restoreOptions.targetRoot);
            restoreResult = RestoreStickBackup::execute(restoreOptions);
        });
        assert(outcome.status == BackupOutcomeStatus::Complete);
        assert(restoreResult.status == RestoreSummary::Status::Failed);
        if (restoreResult.message.find("already writing") == std::string::npos) {
            std::cerr << "unexpected restore message: " << restoreResult.message << "\n";
        }
        assert(restoreResult.message.find("already writing") != std::string::npos);
        std::cout << "case 1 (restore refuses while a backup is writing) OK\n";
    }

    // ---- a compaction refuses while a backup is writing ----
    {
        Fixture f("compact-vs-backup");
        CompactionOutcome compactResult;
        BackupStickOutcome outcome = runBackupWithInjectedCall(f, [&] {
            CompactStickBackupOptions compactOptions;
            compactOptions.archivePath = f.archive;
            compactResult = CompactStickBackup::execute(compactOptions);
        });
        assert(outcome.status == BackupOutcomeStatus::Complete);
        assert(compactResult.status == CompactionOutcome::Status::Failed);
        if (compactResult.message.find("already writing") == std::string::npos) {
            std::cerr << "unexpected compaction message: " << compactResult.message << "\n";
        }
        assert(compactResult.message.find("already writing") != std::string::npos);
        std::cout << "case 2 (compaction refuses while a backup is writing) OK\n";
    }

    // ---- a second backup refuses while the first is writing ----
    {
        Fixture f("backup-vs-backup");
        BackupStickOutcome secondResult;
        BackupStickOutcome outcome = runBackupWithInjectedCall(f, [&] {
            BackupStickOptions secondOptions = f.options;
            secondOptions.onProgress = nullptr;
            secondResult = BackupStick::execute(secondOptions);
        });
        assert(outcome.status == BackupOutcomeStatus::Complete);
        assert(secondResult.status == BackupOutcomeStatus::Failed);
        if (secondResult.message.find("already writing") == std::string::npos) {
            std::cerr << "unexpected second-backup message: " << secondResult.message << "\n";
        }
        assert(secondResult.message.find("already writing") != std::string::npos);
        std::cout << "case 3 (a second backup refuses while the first is writing) OK\n";
    }

    // ---- the lock releases once the backup finishes: a restore then succeeds ----
    {
        Fixture f("lock-releases-after-backup");
        BackupStickOutcome outcome = BackupStick::execute(f.options);
        assert(outcome.status == BackupOutcomeStatus::Complete);

        RestoreOptions restoreOptions;
        restoreOptions.archivePath = f.archive;
        restoreOptions.targetRoot = f.root / "restore-target";
        fs::create_directories(restoreOptions.targetRoot);
        RestoreSummary restoreResult = RestoreStickBackup::execute(restoreOptions);
        if (restoreResult.status != RestoreSummary::Status::Restored) {
            std::cerr << "restore did not succeed after the lock released: " << restoreResult.message << "\n";
            for (const std::string &err : restoreResult.writeErrors) {
                std::cerr << "  write error: " << err << "\n";
            }
        }
        assert(restoreResult.status == RestoreSummary::Status::Restored);
        std::cout << "case 4 (the lock releases after the backup finishes: a restore then succeeds) OK\n";
    }

    std::cout << "All archive_write_lock tests passed.\n";
    return 0;
}
