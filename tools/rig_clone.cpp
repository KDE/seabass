// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: Create or Update Backup USB Stick from one stick onto
// another, the way the Backup USB Stick page runs it -- rig checks C1, C2
// and C5.
//
//   rig_clone <source root> <target root> <backup dir> [--exact]
//   rig_clone <source root> <target root> <backup dir> --cancel-at PERCENT
//   rig_clone <source root> <target root> <backup dir> --expect-too-small
//
// The clone backs the source up into its own archive in <backup dir>
// (<source label>.zip, as the app names it) and restores that archive onto
// the target. A run passes when:
//
// - it reports Cloned;
// - a restore preview of the target against the archive afterwards has no
//   file left to write, and with --exact no extra file left to remove;
// - every catalog file the archive records matches the target's.
//
// With --expect-too-small nothing runs at all: the preview must report
// that the target has no room for the source's library, which is what
// disables the card on the Backup USB Stick page (rig check C6). It
// passes only when the preview refuses for want of room and the source
// really has something to copy. The arithmetic is the app's own --
// CloneStick keeps a free-space margin on top of the bytes to write -- so
// a target whose free space lands inside that margin, which the page
// refuses too, is refused here rather than called big enough. A target
// that is not a mounted drive at all fails earlier, on preview.error, so
// this does not have to second-guess the free-space figure.
//
// With --cancel-at the run is cancelled during the backup stage, once
// PERCENT of the bytes to read are read. It passes when the run ends
// Cancelled before the restore started, the target's files are exactly as
// they were (path, size, modification time), and the archive keeps the
// partial run for the next one to resume.
//
// The target is written. Point it at a test stick.
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
#include <map>
#include <string>
#include <utility>

#include "application/use_cases/clone_stick.hpp"
#include "application/use_cases/restore_stick_backup.hpp"
#include "infrastructure/engine/engine_restore_check.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"
#include "infrastructure/system/rekordbox_process_detector.hpp"
#include "infrastructure/system/stick_hardware_info.hpp"
#include "rig_catalog.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using application::CloneStickOutcome;

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

// Every file and folder under `root`: relative path -> (size, mtime).
using TreeSnapshot = std::map<std::string, std::pair<std::uintmax_t, std::int64_t>>;

TreeSnapshot snapshot(const fs::path &root)
{
    TreeSnapshot tree;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            continue;
        }
        const fs::path relative = fs::relative(it->path(), root, ec);
        std::error_code statError;
        const bool isFile = it->is_regular_file(statError);
        const std::uintmax_t size = isFile ? it->file_size(statError) : 0;
        const auto mtime = it->last_write_time(statError).time_since_epoch().count();
        // Not generic_string(): on Windows that narrows through the
        // process's ANSI code page, which throws for a real file or folder
        // name outside it -- found against a stray real library, not a
        // synthetic one. UTF-8 has no such gap.
        tree[infrastructure::stick_backup::pathToUtf8(relative)] = {size, static_cast<std::int64_t>(mtime)};
    }
    return tree;
}

}  // namespace

int main(int argc, char **argv)
{
    const auto usage = [] {
        std::cerr << "usage: rig_clone <source root> <target root> <backup dir> "
                     "[--exact | --cancel-at PERCENT | --expect-too-small]\n";
        return 2;
    };
    if (argc < 4 || argc > 6) {
        return usage();
    }
    const fs::path source = argv[1];
    const fs::path target = argv[2];
    const fs::path backupDir = argv[3];
    bool exact = false;
    bool expectTooSmall = false;
    int cancelAtPercent = 0;
    if (argc == 5 && std::string(argv[4]) == "--exact") {
        exact = true;
    } else if (argc == 5 && std::string(argv[4]) == "--expect-too-small") {
        expectTooSmall = true;
    } else if (argc == 6 && std::string(argv[4]) == "--cancel-at") {
        cancelAtPercent = std::atoi(argv[5]);
        if (cancelAtPercent < 1 || cancelAtPercent > 99) {
            return usage();
        }
    } else if (argc != 4) {
        return usage();
    }
    bool pass = true;

    try {
        application::CloneStickOptions options;
        options.backup.stickRoot = source;
        options.backup.stickLabel = rig::stickLabelFor(source);
        options.backup.archivePath = backupDir / (options.backup.stickLabel + ".zip");
        options.backup.stickIdentifier =
            infrastructure::system::readStickHardwareInfo(source.string(), options.backup.stickLabel).stickIdentifier;
        options.backup.conflictingProcessProbe = [] { return infrastructure::system::isConflictingDjSoftwareRunning(); };
        options.targetRoot = target;
        options.exact = exact;
        options.libraryCheck = infrastructure::engine::checkRestoredEngineLibrary;
        std::cout << "clone " << source.string() << " -> " << target.string() << " via "
                  << options.backup.archivePath.string() << (exact ? " (exact)" : " (overlay)") << "\n";

        const application::CloneStickPreview preview = application::CloneStick::preview(options);
        if (!preview.error.empty()) {
            std::cout << "preview refused: " << preview.error << "\nRIG RESULT: FAIL\n";
            return 1;
        }
        std::cout << "preview: archive " << (preview.backup.archiveExists ? "exists" : "new")
                  << (preview.archiveCurrent ? " and current" : "") << "; backup reads " << gib(preview.backup.bytesToRead)
                  << " (added " << preview.backup.added << ", changed " << preview.backup.changed << ", removed "
                  << preview.backup.removed << "); source " << gib(preview.sourceBytes) << ", about "
                  << gib(preview.bytesToTarget) << " to the target, " << gib(preview.targetFreeBytes) << " free"
                  << (preview.enoughTargetSpace ? "" : " -- NOT ENOUGH");
        if (preview.restore) {
            std::cout << "; restore would write " << preview.restore->filesToWrite << " files, extras "
                      << preview.restore->extras;
        }
        std::cout << "\n";
        if (expectTooSmall) {
            // Nothing is written in this mode: the preview alone is the
            // check, and the target keeps whatever it holds.
            const bool somethingToCopy = preview.bytesToTarget > 0;
            const bool refused = !preview.enoughTargetSpace && somethingToCopy;
            std::cout << "target space: needs " << gib(preview.bytesToTarget) << " plus the app's margin, has "
                      << gib(preview.targetFreeBytes) << " -> "
                      << (refused ? "too small, as expected"
                                  : (somethingToCopy ? "BIG ENOUGH -- this target cannot check C6"
                                                     : "NOTHING TO COPY -- the source scanned to zero bytes"))
                      << "\n";
            std::cout << "RIG RESULT: " << (refused ? "PASS" : "FAIL") << "\n";
            return refused ? 0 : 1;
        }
        if (!preview.enoughTargetSpace) {
            std::cout << "RIG RESULT: FAIL\n";
            return 1;
        }

        // Read when the copy is done, not now: see
        // BackupStickOptions::readLibraryFingerprint.
        options.backup.readLibraryFingerprint = [&source]() -> std::string {
            const auto fingerprint = rig::fingerprintStick(source);
            return fingerprint ? fingerprint->serialize() : std::string();
        };

        const TreeSnapshot before = cancelAtPercent > 0 ? snapshot(target) : TreeSnapshot{};
        application::CancellationToken cancel;
        options.cancel = cancel;
        int lastStage = -1;
        int lastPercent = -1;
        options.onProgress = [&](const application::CloneProgress &p) {
            const int stage = static_cast<int>(p.stage);
            const std::uint64_t done = p.stage == application::CloneProgress::Stage::Backup ? p.backup.bytesDone
                                                                                            : p.restore.bytesDone;
            const std::uint64_t total = p.stage == application::CloneProgress::Stage::Backup ? p.backup.bytesTotal
                                                                                             : p.restore.bytesTotal;
            const int percent = total > 0 ? static_cast<int>(100 * done / total) : 0;
            if (cancelAtPercent > 0 && !cancel.cancelled() && p.stage == application::CloneProgress::Stage::Backup
                && p.backup.phase == application::BackupProgress::Phase::Reading && total > 0
                && 100 * done >= static_cast<std::uint64_t>(cancelAtPercent) * total) {
                std::cout << timestamp() << " cancelling at " << done << " of " << total << " bytes\n" << std::flush;
                cancel.cancel();
            }
            if (stage != lastStage || percent >= lastPercent + 10) {
                std::cout << timestamp() << " " << (stage == 0 ? "backup" : "restore") << ": " << percent << "%\n"
                          << std::flush;
                lastStage = stage;
                lastPercent = percent;
            }
        };

        std::cout << timestamp() << " cloning\n" << std::flush;
        const CloneStickOutcome outcome = application::CloneStick::execute(options);
        std::cout << timestamp() << " clone " << application::toString(outcome.status) << ": backup read "
                  << gib(outcome.backupBytesRead) << ", restore " << (outcome.restoreStarted ? "started" : "not started");
        if (outcome.restoreStarted) {
            std::cout << ", " << outcome.restore.filesWritten << " written, " << outcome.restore.filesUnchanged
                      << " unchanged, " << outcome.restore.extrasRemoved << " extras removed, "
                      << outcome.restore.writeErrors.size() << " write errors, " << outcome.restore.rejected.size()
                      << " rejected";
        }
        std::cout << "\n";
        if (!outcome.message.empty()) {
            std::cout << "  " << outcome.message << "\n";
        }
        for (const std::string &warning : outcome.restore.warnings) {
            std::cout << "  warning: " << warning << "\n";
        }

        if (cancelAtPercent > 0) {
            const bool stopped = outcome.status == CloneStickOutcome::Status::Cancelled && !outcome.restoreStarted;
            const TreeSnapshot after = snapshot(target);
            const bool untouched = after == before;
            if (!untouched) {
                int shown = 0;
                for (const auto &[path, stat] : after) {
                    auto it = before.find(path);
                    if ((it == before.end() || it->second != stat) && shown++ < 5) {
                        std::cout << "  changed on target: " << path << "\n";
                    }
                }
            }
            const application::StickBackupDescription kept =
                application::RestoreStickBackup::describe(options.backup.archivePath);
            const bool partialKept = kept.error.empty() && kept.entries > 0;
            std::cout << "target " << (untouched ? "untouched" : "CHANGED") << " (" << after.size() << " entries); archive "
                      << (partialKept ? std::string("kept: ") + std::string(infrastructure::stick_backup::toString(kept.status))
                                            + ", " + std::to_string(kept.entries) + " entries"
                                      : std::string("NOT kept") + (kept.error.empty() ? "" : ": " + kept.error))
                      << "\n";
            pass = stopped && untouched && partialKept;
            std::cout << "RIG RESULT: " << (pass ? "PASS" : "FAIL") << "\n";
            return pass ? 0 : 1;
        }

        // A clone of a damaged library is still a clone. The fixture
        // plants dangling rows on purpose, so every run against it comes
        // back "cloned-with-problems" and this check failed for a fault
        // it had faithfully copied: 0 write errors, 0 rejected, 8 of 8
        // catalogs identical, and a red line anyway. What must be clean
        // is the CLONE's own work -- nothing rejected and nothing it
        // could not write. Tracks the source's catalog names and does
        // not have are the source's problem, reported here rather than
        // failed on, and the checks below still require the target to
        // hold what the source held.
        const bool cloneItselfClean = outcome.restore.rejected.empty() && outcome.restore.writeErrors.empty();
        pass = outcome.status == CloneStickOutcome::Status::Cloned
            || (outcome.status == CloneStickOutcome::Status::ClonedWithProblems && cloneItselfClean);
        if (outcome.status == CloneStickOutcome::Status::ClonedWithProblems && cloneItselfClean) {
            const std::size_t missing =
                outcome.restore.missingTrackPaths ? outcome.restore.missingTrackPaths->size() : 0;
            std::cout << "cloned with problems the source already had: " << missing
                      << " track(s) the catalog names and the stick does not hold, nothing rejected, no write errors\n";
        }

        application::RestoreOptions check;
        check.archivePath = options.backup.archivePath;
        check.targetRoot = target;
        check.exact = exact;
        const application::RestorePreview after = application::RestoreStickBackup::preview(check);
        const bool nothingLeft = after.error.empty() && after.filesToWrite == 0 && (!exact || after.extras == 0);
        std::cout << "after: " << after.filesToWrite << " files to write, " << after.extras << " extra on the target"
                  << (exact ? "" : " (kept: overlay)") << " -> " << (nothingLeft ? "target holds the source" : "CHANGES REMAIN")
                  << "\n";
        pass = pass && nothingLeft;

        std::cout << "catalog files against the archive's checksums:\n";
        std::size_t mismatches = 0;
        const std::size_t checked = rig::checkCatalogFiles(options.backup.archivePath, target, mismatches);
        std::cout << "catalogs: " << checked << " checked, " << mismatches << " differ\n";
        pass = pass && checked > 0 && mismatches == 0;
    } catch (const std::exception &e) {
        std::cout << "error: " << e.what() << "\nRIG RESULT: FAIL\n";
        return 1;
    }

    std::cout << "RIG RESULT: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass ? 0 : 1;
}
