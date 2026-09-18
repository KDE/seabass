// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: take a full stick backup the way Back Up This Stick does,
// and prove what it wrote.
//
//   rig_backup <stick root> <archive.zip>
//   rig_backup <stick root> <archive.zip> --expect ADDED,CHANGED,REMOVED,BYTES
//   rig_backup <stick root> <archive.zip> --expect-refused
//   rig_backup <stick root> <archive.zip> --cancel-at PERCENT keep|discard
//
// A run passes when all of these hold:
//
// - It finishes Complete (NothingToDo only when --expect says 0,0,0), and
//   the archive verifies: structure, and every entry's CRC and SHA-256
//   against the manifest -- rig check FB1.
// - The archive is not hollow: the space it occupies on disk is within 5%
//   of its size -- FB2.
// - Nothing is left to back up: a preview straight afterwards finds no
//   added, changed or removed file and no database change.
// - Every catalog file the new manifest records matches the stick's.
// - With --expect, the preview before the run and the run itself report
//   exactly those counts, and the run read exactly BYTES -- FB3.
//
// With --expect-refused the run must be refused because rekordbox or
// Engine DJ is running (FB7), and the archive must be left as it was.
//
// With --cancel-at the run is cancelled once PERCENT of the bytes to read
// are read, and the stopped run is then kept or discarded. Keep (FB4) must
// commit an archive that verifies and leaves the rest to a later run -- a
// plain rig_backup, which then has to complete. Discard (FB5) must leave
// the archive as it was before the run (no file at all for a first backup)
// and no unfinished journal.
//
// The stick is only read. The fingerprint stored in the manifest is taken
// without the duration fill (see rig_catalog.hpp), so the tool leaves no
// cache on the stick.
//
// The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL", and the exit
// code matches.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "application/use_cases/backup_stick.hpp"
#include "infrastructure/system/rekordbox_process_detector.hpp"
#include "infrastructure/system/stick_hardware_info.hpp"
#include "rig_catalog.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using application::BackupOutcomeStatus;

namespace
{

std::string gib(std::uint64_t bytes)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.2f GiB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return text;
}

std::string timestamp()
{
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char text[32];
    std::strftime(text, sizeof text, "%H:%M:%S", std::localtime(&t));
    return text;
}

const char *statusName(BackupOutcomeStatus status)
{
    switch (status) {
    case BackupOutcomeStatus::Complete: return "Complete";
    case BackupOutcomeStatus::NothingToDo: return "NothingToDo";
    case BackupOutcomeStatus::Cancelled: return "Cancelled";
    case BackupOutcomeStatus::KeptPartial: return "KeptPartial";
    case BackupOutcomeStatus::Discarded: return "Discarded";
    case BackupOutcomeStatus::ConflictAborted: return "ConflictAborted";
    case BackupOutcomeStatus::DbTooLarge: return "DbTooLarge";
    case BackupOutcomeStatus::DbUnstable: return "DbUnstable";
    case BackupOutcomeStatus::Failed: return "Failed";
    }
    return "?";
}

struct Expected
{
    std::size_t added = 0;
    std::size_t changed = 0;
    std::size_t removed = 0;
    std::uint64_t bytes = 0;
};

std::optional<Expected> parseExpected(const std::string &text)
{
    Expected e;
    unsigned long long a = 0, c = 0, r = 0, b = 0;
    char tail = 0;
    if (std::sscanf(text.c_str(), "%llu,%llu,%llu,%llu%c", &a, &c, &r, &b, &tail) != 4) {
        return std::nullopt;
    }
    e.added = a;
    e.changed = c;
    e.removed = r;
    e.bytes = b;
    return e;
}

struct ArchiveState
{
    bool exists = false;
    std::uintmax_t size = 0;
    fs::file_time_type mtime;
};

ArchiveState archiveState(const fs::path &archive)
{
    ArchiveState state;
    std::error_code error;
    if (fs::exists(archive, error)) {
        state.exists = true;
        state.size = fs::file_size(archive, error);
        state.mtime = fs::last_write_time(archive, error);
    }
    return state;
}

}  // namespace

int main(int argc, char **argv)
{
    const auto usage = [] {
        std::cerr << "usage: rig_backup <stick root> <archive.zip>"
                     " [--expect ADDED,CHANGED,REMOVED,BYTES | --expect-refused | --cancel-at PERCENT keep|discard]\n";
        return 2;
    };
    if (argc < 3) {
        return usage();
    }
    const fs::path root = argv[1];
    const fs::path archive = argv[2];
    std::optional<Expected> expected;
    bool expectRefused = false;
    int cancelAtPercent = 0;
    bool keepPartial = false;
    if (argc > 3) {
        const std::string mode = argv[3];
        if (argc == 4 && mode == "--expect-refused") {
            expectRefused = true;
        } else if (argc == 5 && mode == "--expect") {
            expected = parseExpected(argv[4]);
        } else if (argc == 6 && mode == "--cancel-at") {
            cancelAtPercent = std::atoi(argv[4]);
            keepPartial = std::string(argv[5]) == "keep";
            if (!keepPartial && std::string(argv[5]) != "discard") {
                cancelAtPercent = 0;
            }
        }
        if (!expectRefused && !expected && (cancelAtPercent < 1 || cancelAtPercent > 99)) {
            return usage();
        }
    }
    bool pass = true;

    try {
        application::BackupStickOptions options;
        options.stickRoot = root;
        options.archivePath = archive;
        options.stickLabel = rig::stickLabelFor(root);
        options.stickIdentifier = infrastructure::system::readStickHardwareInfo(root.string(), options.stickLabel).stickIdentifier;
        std::cout << "stick " << options.stickLabel << " (" << options.stickIdentifier << ") -> " << archive.string() << "\n";

        // The app refuses before it starts when DJ software is running; the
        // probe inside the run only covers software that starts later.
        const std::string blockedBy = infrastructure::system::conflictingDjSoftwareName();
        if (expectRefused || !blockedBy.empty()) {
            const ArchiveState before = archiveState(archive);
            if (blockedBy.empty()) {
                std::cout << "no DJ software detected, so nothing refuses the run\nRIG RESULT: FAIL\n";
                return 1;
            }
            std::cout << "refused: " << blockedBy << " is running\n";
            if (!expectRefused) {
                std::cout << "RIG RESULT: FAIL\n";
                return 1;
            }
            // The refusal happens before BackupStick is reached; what is
            // proved here is that the probe the run would poll agrees, and
            // that nothing touched the archive.
            const bool probeSees = infrastructure::system::isConflictingDjSoftwareRunning();
            const ArchiveState after = archiveState(archive);
            const bool untouched = before.exists == after.exists && before.size == after.size && before.mtime == after.mtime;
            std::cout << "in-run probe " << (probeSees ? "sees it too" : "does NOT see it") << "; archive "
                      << (untouched ? "untouched" : "CHANGED") << "\n";
            pass = probeSees && untouched;
            std::cout << "RIG RESULT: " << (pass ? "PASS" : "FAIL") << "\n";
            return pass ? 0 : 1;
        }

        const application::BackupPreview before = application::BackupStick::preview(options);
        if (!before.error.empty()) {
            std::cout << "preview failed: " << before.error << "\nRIG RESULT: FAIL\n";
            return 1;
        }
        std::cout << "before: " << before.entriesOnStick << " entries on the stick (" << gib(before.stickBytes) << "); archive "
                  << (before.archiveExists ? "exists" : "new") << "; added " << before.added << ", changed " << before.changed
                  << ", removed " << before.removed << ", unchanged " << before.unchanged << ", database "
                  << (before.databaseChanged ? "changed" : "unchanged") << ", " << before.bytesToRead << " bytes to read; "
                  << gib(before.freeBytesAtDestination) << " free" << (before.enoughFreeSpace ? "" : " -- NOT ENOUGH") << ", "
                  << before.skipped.size() << " skipped\n";
        for (const std::string &skipped : before.skipped) {
            std::cout << "  skipped " << skipped << "\n";
        }
        if (expected) {
            const bool counts = before.added == expected->added && before.changed == expected->changed
                && before.removed == expected->removed && before.bytesToRead == expected->bytes;
            std::cout << "preview counts " << (counts ? "as expected" : "NOT as expected") << "\n";
            pass = pass && counts;
        }
        if (!before.enoughFreeSpace) {
            std::cout << "not enough free space for the archive\nRIG RESULT: FAIL\n";
            return 1;
        }

        if (const auto fingerprint = rig::fingerprintStick(root)) {
            options.libraryFingerprint = fingerprint->serialize();
        }
        options.conflictingProcessProbe = [] { return infrastructure::system::isConflictingDjSoftwareRunning(); };
        int lastPhase = -1;
        int lastPercent = -1;
        application::CancellationToken cancel;
        options.cancel = cancel;
        options.onProgress = [&](const application::BackupProgress &p) {
            const int phase = static_cast<int>(p.phase);
            if (cancelAtPercent > 0 && !cancel.cancelled() && p.phase == application::BackupProgress::Phase::Reading
                && p.bytesTotal > 0 && 100 * p.bytesDone >= static_cast<std::uint64_t>(cancelAtPercent) * p.bytesTotal) {
                std::cout << timestamp() << " cancelling at " << p.bytesDone << " of " << p.bytesTotal << " bytes\n" << std::flush;
                cancel.cancel();
            }
            const int percent = p.bytesTotal > 0 ? static_cast<int>(100 * p.bytesDone / p.bytesTotal) : 0;
            if (phase != lastPhase || percent >= lastPercent + 5) {
                static const char *names[] = {"scanning", "reading", "database", "writing", "verifying"};
                std::cout << timestamp() << " " << names[phase] << ": " << p.filesDone << "/" << p.filesTotal << " files, "
                          << percent << "%\n"
                          << std::flush;
                lastPhase = phase;
                lastPercent = percent;
            }
        };

        const ArchiveState beforeRun = archiveState(archive);
        std::cout << timestamp() << " backing up\n" << std::flush;
        const application::BackupStickOutcome outcome = application::BackupStick::execute(options);
        std::cout << timestamp() << " backup " << statusName(outcome.status) << ": added " << outcome.added << ", changed "
                  << outcome.changed << ", removed " << outcome.removed << ", carried " << outcome.carried << ", read "
                  << outcome.bytesRead << " bytes (" << gib(outcome.bytesRead) << "), archive " << gib(outcome.archiveBytes)
                  << ", dead " << outcome.deadBytes << " bytes, database " << (outcome.databaseCaptured ? "captured" : "not captured")
                  << "\n";
        if (!outcome.message.empty()) {
            std::cout << "  " << outcome.message << "\n";
        }
        for (const std::string &warning : outcome.warnings) {
            std::cout << "  warning: " << warning << "\n";
        }
        if (cancelAtPercent > 0) {
            if (outcome.status != BackupOutcomeStatus::Cancelled || !outcome.pending) {
                std::cout << "expected a stopped run waiting for keep or discard\nRIG RESULT: FAIL\n";
                return 1;
            }
            const application::BackupStickOutcome decided = keepPartial ? outcome.pending->keep() : outcome.pending->discard();
            std::cout << timestamp() << " " << (keepPartial ? "keep" : "discard") << ": " << statusName(decided.status)
                      << ", added " << decided.added << ", archive " << gib(decided.archiveBytes) << "\n";
            if (!decided.message.empty()) {
                std::cout << "  " << decided.message << "\n";
            }
            // An empty journal is a settled archive; only a non-empty one is
            // an unfinished update the next open would have to roll back.
            std::error_code error;
            const fs::path journal = application::BackupStick::journalPathFor(archive);
            const bool journalPending = fs::exists(journal, error) && fs::file_size(journal, error) > 0;
            const ArchiveState afterDecision = archiveState(archive);
            std::cout << "archive " << (afterDecision.exists ? "exists, " + std::to_string(afterDecision.size) + " bytes" : "absent")
                      << " (before the run: "
                      << (beforeRun.exists ? "existed, " + std::to_string(beforeRun.size) + " bytes" : "absent") << "); journal "
                      << (journalPending ? "UNFINISHED" : "settled") << "\n";
            if (keepPartial) {
                const application::VerifyOutcome verified = application::BackupStick::verify(archive);
                std::cout << timestamp() << " verify " << (verified.ok ? "VERIFIED" : "FAILED") << ": " << verified.entriesChecked
                          << " entries, status " << infrastructure::stick_backup::toString(verified.status) << "\n";
                // A fresh token: the run's own is cancelled, and a preview
                // walking with it stops at once and reports nothing left.
                options.cancel = application::CancellationToken::none();
                const application::BackupPreview rest = application::BackupStick::preview(options);
                std::cout << "left for the next run: added " << rest.added << ", changed " << rest.changed << ", "
                          << rest.bytesToRead << " bytes to read\n";
                pass = decided.status == BackupOutcomeStatus::KeptPartial && !journalPending && verified.ok && rest.error.empty()
                    && rest.added + rest.changed > 0;
            } else {
                const bool asBefore = afterDecision.exists == beforeRun.exists && afterDecision.size == beforeRun.size;
                pass = decided.status == BackupOutcomeStatus::Discarded && !journalPending && asBefore;
            }
            std::cout << "RIG RESULT: " << (pass ? "PASS" : "FAIL") << "\n";
            return pass ? 0 : 1;
        }
        const bool nothingExpected = expected && expected->added == 0 && expected->changed == 0 && expected->removed == 0;
        const bool finished = outcome.status == BackupOutcomeStatus::Complete
            || (nothingExpected && outcome.status == BackupOutcomeStatus::NothingToDo);
        pass = pass && finished;
        if (expected) {
            const bool counts = outcome.added == expected->added && outcome.changed == expected->changed
                && outcome.removed == expected->removed && outcome.bytesRead == expected->bytes;
            std::cout << "run counts " << (counts ? "as expected" : "NOT as expected") << "\n";
            pass = pass && counts;
        }

        std::cout << timestamp() << " verifying the archive\n" << std::flush;
        const application::VerifyOutcome verified = application::BackupStick::verify(archive);
        std::cout << timestamp() << " verify " << (verified.ok ? "VERIFIED" : "FAILED") << ": " << verified.entriesChecked
                  << " entries, " << gib(verified.bytesChecked) << ", status "
                  << infrastructure::stick_backup::toString(verified.status) << "\n";
        if (!verified.error.empty()) {
            std::cout << "  " << verified.error << "\n";
        }
        for (const std::string &failure : verified.failures) {
            std::cout << "  bad entry: " << failure << "\n";
        }
        pass = pass && verified.ok;

#ifndef _WIN32
        struct stat st
        {
        };
        if (::stat(archive.c_str(), &st) == 0) {
            const auto allocated = static_cast<std::uint64_t>(st.st_blocks) * 512u;
            const auto size = static_cast<std::uint64_t>(st.st_size);
            const double ratio = size > 0 ? static_cast<double>(allocated) / static_cast<double>(size) : 0.0;
            const bool solid = ratio >= 0.95;
            std::cout << "on disk: " << size << " bytes, " << allocated << " allocated (" << ratio * 100.0 << "%) -> "
                      << (solid ? "not hollow" : "HOLLOW") << "\n";
            pass = pass && solid;
        } else {
            std::cout << "on disk: cannot stat the archive\n";
            pass = false;
        }
#endif

        const application::BackupPreview after = application::BackupStick::preview(options);
        const bool nothingLeft = after.error.empty() && after.added == 0 && after.changed == 0 && after.removed == 0
            && !after.databaseChanged;
        std::cout << "after: added " << after.added << ", changed " << after.changed << ", removed " << after.removed
                  << ", database " << (after.databaseChanged ? "changed" : "unchanged") << " -> "
                  << (nothingLeft ? "nothing left to back up" : "CHANGES REMAIN") << "\n";
        if (!after.error.empty()) {
            std::cout << "  " << after.error << "\n";
        }
        pass = pass && nothingLeft;

        std::cout << "catalog files against the new manifest's checksums:\n";
        std::size_t mismatches = 0;
        const std::size_t checked = rig::checkCatalogFiles(archive, root, mismatches);
        const bool catalogsMatch = checked > 0 && mismatches == 0;
        std::cout << "catalogs: " << checked << " checked, " << mismatches << " differ\n";
        pass = pass && catalogsMatch;
    } catch (const std::exception &e) {
        std::cout << "error: " << e.what() << "\nRIG RESULT: FAIL\n";
        return 1;
    }

    std::cout << "RIG RESULT: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass ? 0 : 1;
}
