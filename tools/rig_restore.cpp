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
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"
#include "infrastructure/hashing/sha256.hpp"

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

// The stick's library fingerprint, for information: rekordbox and Engine
// tracks together, as the app fingerprints them -- but without filling in
// missing lengths. The fill probes audio files and writes its results to a
// cache on the stick, and a check must not change what it checks: the
// first rig run left that cache behind as two "extras".
std::optional<domain::LibraryFingerprint> fingerprintStick(const fs::path &root)
{
    std::vector<domain::Track> tracks;
    bool anyRead = false;
    const fs::path pioneer = root / "PIONEER";
    if (fs::exists(pioneer / "rekordbox" / "export.pdb")) {
        infrastructure::rekordbox::KaitaiRekordboxReader reader(pioneer.string());
        std::vector<domain::Track> read = application::ScanLibrary(reader).execute();
        std::cout << "  rekordbox: " << read.size() << " tracks\n";
        tracks.insert(tracks.end(), read.begin(), read.end());
        anyRead = true;
    }
    const fs::path engine = root / "Engine Library";
    if (fs::exists(engine / "Database2" / "m.db") || fs::exists(engine / "m.db")) {
        infrastructure::engine::LibdjinteropEngineReader reader(engine.string());
        std::vector<domain::Track> read = application::ScanLibrary(reader).execute();
        std::cout << "  engine: " << read.size() << " tracks\n";
        tracks.insert(tracks.end(), read.begin(), read.end());
        anyRead = true;
    }
    if (!anyRead) {
        return std::nullopt;
    }
    return domain::fingerprintLibrary(tracks);
}

bool isCatalogFile(const std::string &path)
{
    const auto endsWith = [&path](std::string_view tail) {
        return path.size() >= tail.size() && path.compare(path.size() - tail.size(), tail.size(), tail) == 0;
    };
    if (path.rfind("PIONEER/rekordbox/", 0) == 0) {
        return endsWith(".pdb") || endsWith(".db") || endsWith(".db-wal");
    }
    if (path.rfind("Engine Library/Database2/", 0) == 0) {
        return endsWith(".db") || endsWith(".db-wal");
    }
    return false;
}

// Every catalog file the manifest lists, hashed on the stick and compared.
// Returns the number checked; mismatches are printed and counted in `bad`.
std::size_t checkCatalogFiles(const fs::path &archive, const fs::path &root, std::size_t &bad)
{
    namespace sb = infrastructure::stick_backup;
    sb::PosixArchiveFile file(archive, sb::PosixArchiveFile::OpenMode::ReadOnly);
    sb::Zip64Reader reader = sb::Zip64Reader::open(file);
    const std::optional<std::size_t> index = reader.findEntry(sb::ManifestEntryName);
    if (!index) {
        throw std::runtime_error("the archive has no manifest");
    }
    std::string manifestError;
    const std::optional<sb::BackupManifest> manifest = sb::BackupManifest::parse(reader.readEntryToString(*index), &manifestError);
    if (!manifest) {
        throw std::runtime_error("the manifest is damaged: " + manifestError);
    }
    std::size_t checked = 0;
    for (const sb::ManifestRow &row : manifest->rows) {
        if (row.kind != sb::ManifestRow::Kind::File || !isCatalogFile(row.path)) {
            continue;
        }
        ++checked;
        const fs::path onStick = root / row.path;
        std::ifstream in(onStick, std::ios::binary);
        if (!in) {
            std::cout << "  MISSING " << row.path << "\n";
            ++bad;
            continue;
        }
        infrastructure::hashing::Sha256 hasher;
        std::vector<char> buffer(1 << 20);
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize got = in.gcount();
            if (got > 0) {
                hasher.update(std::span<const std::byte>(reinterpret_cast<const std::byte *>(buffer.data()), static_cast<std::size_t>(got)));
            }
        }
        const bool same = hasher.finish() == row.sha256;
        std::cout << "  " << (same ? "same    " : "DIFFERS ") << row.path << "\n";
        if (!same) {
            ++bad;
        }
    }
    return checked;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4 || (argc == 4 && std::string(argv[3]) != "--execute")) {
        std::cerr << "usage: rig_restore <archive.zip> <stick root> [--execute]\n";
        return 2;
    }
    const fs::path archive = argv[1];
    const fs::path root = argv[2];
    const bool execute = argc == 4;
    bool pass = true;

    try {
        const application::StickBackupDescription description = application::RestoreStickBackup::describe(archive);
        if (!description.error.empty()) {
            std::cout << "archive unreadable: " << description.error << "\nRIG RESULT: FAIL\n";
            return 1;
        }
        std::cout << "archive " << archive.filename().string() << ": stick " << description.stickLabel << " ("
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
                  << gib(before.freeBytesAtTarget) << " free" << (before.enoughFreeSpace ? "" : " -- NOT ENOUGH")
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
        const std::size_t catalogsChecked = checkCatalogFiles(archive, root, catalogMismatches);
        const bool catalogsMatch = catalogsChecked > 0 && catalogMismatches == 0;
        std::cout << "catalogs: " << catalogsChecked << " checked, " << catalogMismatches << " differ -> "
                  << (catalogsMatch ? "the stick holds the backup's library" : "LIBRARY DIFFERS") << "\n";
        pass = pass && catalogsMatch;

        const auto expected = domain::LibraryFingerprint::parse(description.libraryFingerprint);
        std::cout << "fingerprint of the stick (information only):\n";
        const auto live = fingerprintStick(root);
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
