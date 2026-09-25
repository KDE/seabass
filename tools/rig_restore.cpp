// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: put a test stick into a known state from a reference full
// stick backup, and prove it got there.
//
//   rig_restore <archive.zip> <stick root>             preview and check only
//   rig_restore <archive.zip> <stick root> --execute   exact restore, then check
//
// "Proved" means two independent things, both of which have to hold:
//
// - Nothing is left to restore. The same archive previewed against the
//   stick afterwards has no file to write and no extra file to remove --
//   the zero-changes criterion of rig checks B1 and B3.
// - The library on the stick is the library the backup holds: every catalog
//   database file (rekordbox's export.pdb and exportLibrary.db, Engine's
//   Database2/*.db and their -wal) matches the SHA-256 the manifest recorded
//   for it -- check B2. This is checked before anything reads the catalogs.
//
// The library fingerprint is printed too, for information only: it depends
// on the fingerprinting code of the day (backups taken before 2026-09-07
// fingerprinted without the duration fill the app does now), so an older
// manifest's fingerprint cannot be expected to match today's reading.
//
// It never writes to the archive: preview and describe open it read-only,
// and the restore only reads from it. The stick is overwritten (exact mode
// removes whatever the backup does not hold), so point it at a test stick.
//
// The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL", and the exit
// code matches, so a runner can record the result without parsing more.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>


#include "application/use_cases/restore_stick_backup.hpp"
#include "application/use_cases/scan_library.hpp"
#include "domain/library_fingerprint.hpp"
#include "domain/track.hpp"
#include "infrastructure/engine/engine_restore_check.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "rig_catalog.hpp"

namespace fs = std::filesystem;
using namespace seabass;

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
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    char text[32];
    std::strftime(text, sizeof text, "%H:%M:%S", std::localtime(&t));
    return text;
}

const char *verdictName(domain::FingerprintSimilarity::Verdict verdict)
{
    switch (verdict) {
    case domain::FingerprintSimilarity::Verdict::Same: return "same library";
    case domain::FingerprintSimilarity::Verdict::SameCollectionDifferentState: return "same tracks, different cues";
    case domain::FingerprintSimilarity::Verdict::Different: return "different library";
    case domain::FingerprintSimilarity::Verdict::Unknown: return "cannot tell";
    }
    return "cannot tell";
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4 || (argc == 4 && std::string(argv[3]) != "--execute")) {
        std::cerr << "usage: rig_restore <archive.zip> <stick root> [--execute]\n";
        return 2;
    }
    const fs::path archive = pathFromUtf8(argv[1]);
    const fs::path root = pathFromUtf8(argv[2]);
    const bool execute = argc == 4;
    bool pass = true;

    try {
        const application::StickBackupDescription description = application::RestoreStickBackup::describe(archive);
        if (!description.error.empty()) {
            std::cout << "archive unreadable: " << description.error << "\nRIG RESULT: FAIL\n";
            return 1;
        }
        std::cout << "archive " << pathToUtf8(archive.filename()) << ": stick " << description.stickLabel << " ("
                  << description.stickIdentifier << "), "
                  << infrastructure::stick_backup::toString(description.status) << ", " << description.entries
                  << " entries, " << gib(description.archiveBytes) << "\n";

        application::RestoreOptions options;
        options.archivePath = archive;
        options.targetRoot = root;
        options.exact = true;
        options.libraryCheck = infrastructure::engine::checkRestoredEngineLibrary;

        const application::RestorePreview before = application::RestoreStickBackup::preview(options);
        if (!before.error.empty()) {
            std::cout << "preview failed: " << before.error << "\nRIG RESULT: FAIL\n";
            return 1;
        }
        std::cout << "before: " << before.filesToWrite << " files to write (" << gib(before.bytesToWrite) << "), "
                  << before.filesUnchanged << " unchanged, " << before.extras << " extra on the stick, "
                  << gib(before.freeBytesAtTarget) << " free" << (before.enoughFreeSpace ? "" : " (NOT ENOUGH)")
                  << ", " << before.rejected.size() << " rejected\n";

        if (execute) {
            if (!before.enoughFreeSpace) {
                std::cout << "not enough free space on the stick\nRIG RESULT: FAIL\n";
                return 1;
            }
            int lastPercent = -1;
            int lastPhase = -1;
            options.onProgress = [&](const application::RestoreProgress &p) {
                const int phase = static_cast<int>(p.phase);
                const int percent = p.bytesTotal > 0 ? static_cast<int>(100 * p.bytesDone / p.bytesTotal) : 0;
                if (phase != lastPhase || percent >= lastPercent + 5) {
                    static const char *names[] = {"analyzing", "writing", "removing extras", "checking database"};
                    std::cout << timestamp() << " " << names[phase] << ": " << p.filesDone << "/" << p.filesTotal
                              << " files, " << percent << "%\n"
                              << std::flush;
                    lastPhase = phase;
                    lastPercent = percent;
                }
            };
            std::cout << timestamp() << " restoring (exact)\n" << std::flush;
            const application::RestoreSummary summary = application::RestoreStickBackup::execute(options);
            const bool restored = summary.status == application::RestoreSummary::Status::Restored;
            std::cout << timestamp() << " restore " << (restored ? "finished" : "did NOT finish cleanly") << ": "
                      << summary.filesWritten << " written, " << summary.filesUnchanged << " unchanged, "
                      << summary.extrasRemoved << " extras removed, " << gib(summary.bytesWritten) << "\n";
            if (!summary.message.empty()) {
                std::cout << "  " << summary.message << "\n";
            }
            for (const auto &[entry, reason] : summary.rejected) {
                std::cout << "  rejected " << entry << ": " << reason << "\n";
            }
            for (const std::string &error : summary.writeErrors) {
                std::cout << "  write error: " << error << "\n";
            }
            if (summary.missingTrackPaths && !summary.missingTrackPaths->empty()) {
                std::cout << "  " << summary.missingTrackPaths->size() << " tracks reference missing files\n";
            }
            for (const std::string &warning : summary.warnings) {
                std::cout << "  warning: " << warning << "\n";
            }
            // Tracks pointing at files the backup never held come from the
            // library itself; only a failed write or a rejected entry is the
            // restore's fault.
            pass = pass && summary.writeErrors.empty() && summary.rejected.empty()
                && summary.status != application::RestoreSummary::Status::Failed
                && summary.status != application::RestoreSummary::Status::Cancelled;
            (void)restored;
        }

        const application::RestorePreview after = application::RestoreStickBackup::preview(options);
        const bool zeroChanges = after.error.empty() && after.filesToWrite == 0 && after.extras == 0;
        std::cout << "after: " << after.filesToWrite << " files to write, " << after.extras
                  << " extra on the stick -> " << (zeroChanges ? "zero changes" : "CHANGES REMAIN") << "\n";
        pass = pass && zeroChanges;

        std::cout << "catalog files against the manifest's checksums:\n";
        std::size_t catalogMismatches = 0;
        const std::size_t catalogsChecked = rig::checkCatalogFiles(archive, root, catalogMismatches);
        const bool catalogsMatch = catalogsChecked > 0 && catalogMismatches == 0;
        std::cout << "catalogs: " << catalogsChecked << " checked, " << catalogMismatches << " differ -> "
                  << (catalogsMatch ? "the stick holds the backup's library" : "LIBRARY DIFFERS") << "\n";
        pass = pass && catalogsMatch;

        const auto expected = domain::LibraryFingerprint::parse(description.libraryFingerprint);
        std::cout << "fingerprint of the stick (information only):\n";
        const auto live = rig::fingerprintStick(root);
        if (!expected || !live) {
            std::cout << "fingerprint: " << (expected ? "" : "none in the manifest; ")
                      << (live ? "" : "no readable library on the stick") << "\n";
        } else {
            const domain::FingerprintSimilarity similarity = domain::compareFingerprints(*expected, *live);
            const bool countsMatch = expected->trackCount == live->trackCount
                && expected->cuedTrackCount == live->cuedTrackCount && expected->playlistCount == live->playlistCount;
            std::cout << "  backup: " << expected->trackCount << " tracks, " << expected->cuedTrackCount << " cued, "
                      << expected->playlistCount << " playlists\n"
                      << "  stick:  " << live->trackCount << " tracks, " << live->cuedTrackCount << " cued, "
                      << live->playlistCount << " playlists\n"
                      << "  verdict: " << verdictName(similarity.verdict) << " (track overlap "
                      << similarity.trackOverlap << ", cue overlap " << similarity.cueOverlap << ")\n";
            (void)countsMatch;
        }
    } catch (const std::exception &e) {
        std::cout << "error: " << e.what() << "\nRIG RESULT: FAIL\n";
        return 1;
    }

    std::cout << "RIG RESULT: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass ? 0 : 1;
}
