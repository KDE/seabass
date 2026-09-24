// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: compact a full stick backup and prove it reclaimed exactly
// what it said it would -- rig check FB6.
//
//   rig_compact <archive.zip>
//
// Passes when compaction had something to reclaim, reports Compacted,
// the file shrank by exactly the reclaimable bytes the preflight reported
// (what the compact dialog promises) and ended at the predicted size, no
// temporary copy is left behind, and the compacted archive verifies with
// as many entries as before. The dead bytes are printed beside it: they
// differ from what compacting frees when entries move across 4 GiB.
//
// Needs the preflight's reclaimableBytes (branch compact-exact-reclaim).
//
// Never touches a stick. The archive is rewritten, so point it at a test
// backup, never at a reference.
//
// The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL", and the exit
// code matches.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

#include "application/use_cases/backup_stick.hpp"
#include "application/use_cases/compact_stick_backup.hpp"
#include "application/use_cases/restore_stick_backup.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;

namespace
{

std::string timestamp()
{
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char text[32];
    std::strftime(text, sizeof text, "%H:%M:%S", std::localtime(&t));
    return text;
}

const char *statusName(application::CompactionOutcome::Status status)
{
    using Status = application::CompactionOutcome::Status;
    switch (status) {
    case Status::Compacted: return "Compacted";
    case Status::NothingToReclaim: return "NothingToReclaim";
    case Status::NotEnoughSpace: return "NotEnoughSpace";
    case Status::Cancelled: return "Cancelled";
    case Status::Failed: return "Failed";
    }
    return "?";
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: rig_compact <archive.zip>\n";
        return 2;
    }
    const fs::path archive = pathFromUtf8(argv[1]);
    bool pass = true;

    try {
        const application::StickBackupDescription described = application::RestoreStickBackup::describe(archive);
        if (!described.error.empty()) {
            std::cout << "archive unreadable: " << described.error << "\nRIG RESULT: FAIL\n";
            return 1;
        }
        const application::CompactionPreflight preflight = application::CompactStickBackup::preflight(archive);
        std::cout << "before: " << described.entries << " entries; " << preflight.archiveBytes << " bytes, " << preflight.liveBytes
                  << " live, " << preflight.deadBytes << " dead (" << preflight.deadRatio * 100.0 << "%), reclaimable "
                  << preflight.reclaimableBytes << " (compacted " << preflight.compactedBytes << "), suggested "
                  << (preflight.suggested ? "yes" : "no") << "; needs " << preflight.requiredFreeBytes << " free, "
                  << preflight.availableFreeBytes << " available\n";
        if (!preflight.error.empty()) {
            std::cout << "preflight failed: " << preflight.error << "\nRIG RESULT: FAIL\n";
            return 1;
        }
        if (preflight.reclaimableBytes == 0) {
            std::cout << "nothing to reclaim, so nothing to prove\nRIG RESULT: FAIL\n";
            return 1;
        }
        const std::uintmax_t sizeBefore = fs::file_size(archive);

        application::CompactStickBackupOptions options;
        options.archivePath = archive;
        std::cout << timestamp() << " compacting\n" << std::flush;
        const application::CompactionOutcome outcome = application::CompactStickBackup::execute(options);
        std::cout << timestamp() << " " << statusName(outcome.status) << ": " << outcome.bytesBefore << " -> " << outcome.bytesAfter
                  << " bytes\n";
        if (!outcome.message.empty()) {
            std::cout << "  " << outcome.message << "\n";
        }
        pass = pass && outcome.status == application::CompactionOutcome::Status::Compacted;

        const std::uintmax_t sizeAfter = fs::file_size(archive);
        const std::uintmax_t shrank = sizeBefore >= sizeAfter ? sizeBefore - sizeAfter : 0;
        const bool exact = sizeAfter <= sizeBefore && shrank == preflight.reclaimableBytes && sizeAfter == preflight.compactedBytes;
        std::cout << "file: " << sizeBefore << " -> " << sizeAfter << " bytes, shrank by " << shrank << ", preflight said "
                  << preflight.reclaimableBytes << " -> " << (exact ? "exactly as previewed" : "NOT as previewed")
                  << " (dead bytes " << preflight.deadBytes << ", "
                  << static_cast<long long>(shrank) - static_cast<long long>(preflight.deadBytes) << " from what was freed)\n";
        pass = pass && exact;

        const bool tempGone = !fs::exists(application::CompactStickBackup::temporaryPathFor(archive));
        std::cout << "temporary copy " << (tempGone ? "gone" : "LEFT BEHIND") << "\n";
        pass = pass && tempGone;

        std::cout << timestamp() << " verifying the archive\n" << std::flush;
        const application::VerifyOutcome verified = application::BackupStick::verify(archive);
        const application::StickBackupDescription after = application::RestoreStickBackup::describe(archive);
        const bool sameEntries = after.error.empty() && after.entries == described.entries;
        std::cout << timestamp() << " verify " << (verified.ok ? "VERIFIED" : "FAILED") << ": " << verified.entriesChecked
                  << " entries checked; " << after.entries << " entries listed (" << (sameEntries ? "same" : "DIFFERENT") << ")\n";
        if (!verified.error.empty()) {
            std::cout << "  " << verified.error << "\n";
        }
        for (const std::string &failure : verified.failures) {
            std::cout << "  bad entry: " << failure << "\n";
        }
        pass = pass && verified.ok && sameEntries;
    } catch (const std::exception &e) {
        std::cout << "error: " << e.what() << "\nRIG RESULT: FAIL\n";
        return 1;
    }

    std::cout << "RIG RESULT: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass ? 0 : 1;
}
