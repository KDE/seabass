// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: the emergency copy of a damaged stick, and restoring it --
// issue #36 on real hardware.
//
//   rig_salvage backup <stick root> <archive.zip>
//   rig_salvage restore <archive.zip> <target root>
//
// backup takes a full stick backup the way Back Up This Stick does, with
// sourceReadOnly decided the way the page decides it: from the mount. The
// stick has to be mounted read-only already -- a salvage run off a
// read-write stick would test the healthy path, so that is a FAIL, not a
// skip. It prints every file the run could only keep in part and every
// warning, and the archive has to verify.
//
// restore writes such a backup onto a target (overlay, never exact) and
// prints every entry the restore reports as partial, and whether it was
// written or a whole copy was left in place.
//
// Whether the salvaged bytes are the file's own bytes is not decided here:
// that needs the files as they were before the damage, which only the
// person running the round has. See the live-test notes on #36.
//
// The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL", and the exit
// code matches.

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

#include "application/use_cases/backup_stick.hpp"
#include "application/use_cases/restore_stick_backup.hpp"
#include "infrastructure/engine/engine_restore_check.hpp"
#include "infrastructure/media/filesystem_health.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"
#include "infrastructure/system/rekordbox_process_detector.hpp"
#include "infrastructure/system/stick_hardware_info.hpp"
#include "rig_catalog.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using application::BackupOutcomeStatus;

namespace
{

int backup(const fs::path &root, const fs::path &archive)
{
    const std::string rootUtf8 = pathToUtf8(root);
    const bool readOnly = infrastructure::media::isMountedReadOnly(rootUtf8);
    std::cout << "stick " << rootUtf8 << " is mounted " << (readOnly ? "read-only" : "READ-WRITE") << "\n";
    if (!readOnly) {
        std::cout << "a salvage run needs a stick the kernel has already made read-only\nRIG RESULT: FAIL\n";
        return 1;
    }
    application::BackupStickOptions options;
    options.stickRoot = root;
    options.archivePath = archive;
    options.stickLabel = rig::stickLabelFor(root);
    options.stickIdentifier = infrastructure::system::readStickHardwareInfo(rootUtf8, options.stickLabel).stickIdentifier;
    options.sourceReadOnly = readOnly;
    options.conflictingProcessProbe = [] { return infrastructure::system::isConflictingDjSoftwareRunning(); };

    const application::BackupStickOutcome outcome = application::BackupStick::execute(options);
    const bool complete = outcome.status == BackupOutcomeStatus::Complete;
    std::cout << "backup " << (complete ? "Complete" : "NOT Complete (" + std::to_string(static_cast<int>(outcome.status)) + ")")
              << ": added " << outcome.added << ", read " << outcome.bytesRead << " bytes, database "
              << (outcome.databaseCaptured ? "captured" : "not captured") << "\n";
    if (!outcome.message.empty()) {
        std::cout << "  message: " << outcome.message << "\n";
    }
    for (const auto &salvaged : outcome.salvaged) {
        std::cout << "  salvaged: " << salvaged.path << ": " << salvaged.bytesSalvaged << " of " << salvaged.expectedSize
                  << " bytes (" << salvaged.reason << ")\n";
    }
    for (const std::string &warning : outcome.warnings) {
        std::cout << "  warning: " << warning << "\n";
    }
    const application::VerifyOutcome verified = application::BackupStick::verify(archive);
    std::cout << "verify " << (verified.ok ? "VERIFIED" : "FAILED") << ": " << verified.entriesChecked << " entries\n";
    for (const std::string &failure : verified.failures) {
        std::cout << "  bad entry: " << failure << "\n";
    }
    // Every file the archive holds against the stick's own size. A row
    // shorter than the file on the stick has to say so (salvagedFromSize);
    // one that does not is a truncated file presented as whole, and a
    // restore will write it over a good copy.
    namespace sb = infrastructure::stick_backup;
    sb::PosixArchiveFile file(archive, sb::PosixArchiveFile::OpenMode::ReadOnly);
    sb::Zip64Reader reader = sb::Zip64Reader::open(file);
    const std::optional<std::size_t> index = reader.findEntry(sb::ManifestEntryName);
    const std::optional<sb::BackupManifest> manifest =
        index ? sb::BackupManifest::parse(reader.readEntryToString(*index)) : std::nullopt;
    std::size_t rowsChecked = 0;
    std::size_t silentlyShort = 0;
    for (const sb::ManifestRow &row : manifest ? manifest->rows : std::vector<sb::ManifestRow>{}) {
        std::error_code error;
        // The manifest key is UTF-8 with forward slashes.
        const std::uint64_t onStick = fs::file_size(root / pathFromUtf8(row.path), error);
        if (row.kind != sb::ManifestRow::Kind::File || error) {
            continue;
        }
        ++rowsChecked;
        if (row.size != onStick && row.salvagedFromSize != onStick) {
            ++silentlyShort;
            std::cout << "  SILENTLY SHORT: " << row.path << ": archive holds " << row.size << " of " << onStick
                      << " bytes and the manifest calls it whole\n";
        }
    }
    std::cout << "manifest rows against the stick: " << rowsChecked << " checked, " << silentlyShort << " silently short\n";
    const bool pass = complete && verified.ok && !outcome.salvaged.empty() && rowsChecked > 0 && silentlyShort == 0;
    if (outcome.salvaged.empty()) {
        std::cout << "nothing was salvaged: the damage never reached a read\n";
    }
    std::cout << "RIG RESULT: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass ? 0 : 1;
}

int restore(const fs::path &archive, const fs::path &target)
{
    application::RestoreOptions options;
    options.archivePath = archive;
    options.targetRoot = target;
    options.libraryCheck = infrastructure::engine::checkRestoredEngineLibrary;
    const application::RestorePreview preview = application::RestoreStickBackup::preview(options);
    if (!preview.error.empty()) {
        std::cout << "preview failed: " << preview.error << "\nRIG RESULT: FAIL\n";
        return 1;
    }
    std::cout << "preview: emergency copy " << (preview.sourceReadOnly ? "yes" : "NO") << ", " << preview.filesToWrite
              << " to write, " << preview.filesUnchanged << " unchanged\n";
    const application::RestoreSummary summary = application::RestoreStickBackup::execute(options);
    static const char *names[] = {"Restored", "RestoredWithProblems", "Cancelled", "Failed"};
    std::cout << "restore " << names[static_cast<int>(summary.status)] << ": " << summary.filesWritten << " written, "
              << summary.filesUnchanged << " unchanged\n  message: " << summary.message << "\n";
    for (const auto &partial : summary.partial) {
        std::cout << "  partial: " << partial.path << ": " << partial.bytesAvailable << " of " << partial.originalSize
                  << " bytes, " << (partial.written ? "WRITTEN" : "whole copy left in place") << "\n";
    }
    for (const std::string &warning : summary.warnings) {
        std::cout << "  warning: " << warning << "\n";
    }
    for (const std::string &error : summary.writeErrors) {
        std::cout << "  write error: " << error << "\n";
    }
    if (summary.missingTrackPaths) {
        std::cout << "  engine library: " << summary.missingTrackPaths->size() << " referenced tracks missing\n";
    }
    const bool pass = preview.sourceReadOnly && !summary.partial.empty()
        && summary.status == application::RestoreSummary::Status::RestoredWithProblems;
    std::cout << "RIG RESULT: " << (pass ? "PASS" : "FAIL") << "\n";
    return pass ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::cerr << "usage: rig_salvage backup <stick root> <archive.zip>\n"
                     "       rig_salvage restore <archive.zip> <target root>\n";
        return 2;
    }
    const std::string mode = argv[1];
    try {
        if (mode == "backup") {
            return backup(pathFromUtf8(argv[2]), pathFromUtf8(argv[3]));
        }
        if (mode == "restore") {
            return restore(pathFromUtf8(argv[2]), pathFromUtf8(argv[3]));
        }
    } catch (const std::exception &e) {
        std::cout << "error: " << e.what() << "\nRIG RESULT: FAIL\n";
        return 1;
    }
    std::cerr << "unknown mode " << mode << "\n";
    return 2;
}
