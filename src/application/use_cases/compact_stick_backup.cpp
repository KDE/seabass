#include "application/use_cases/compact_stick_backup.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>

#include "infrastructure/backup/stick_write_lock.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/stick_backup/archive_compactor.hpp"
#include "infrastructure/stick_backup/archive_journal.hpp"
#include "infrastructure/stick_backup/archive_recovery.hpp"
#include "infrastructure/stick_backup/archive_stats.hpp"
#include "infrastructure/stick_backup/archive_updater.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"
#include "infrastructure/stick_backup/zip_format.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace seabass::application
{

namespace fs = std::filesystem;
using namespace infrastructure::stick_backup;

namespace
{

struct Opened
{
    std::unique_ptr<PosixArchiveFile> archive;
    std::unique_ptr<PosixArchiveFile> journal;
    std::optional<Zip64Reader> reader;
    std::optional<BackupManifest> manifest;
    std::string error;

    // recover: replay/discard the journal, which truncates the archive to
    // the last committed length. Only the caller holding the archive lock
    // may do that; a preflight that runs while another instance appends
    // would cut its run short and, on Windows, zero-fill the rest of it.
    bool open(const fs::path &archivePath, bool recover)
    {
        try {
            archive = std::make_unique<PosixArchiveFile>(archivePath, recover ? PosixArchiveFile::OpenMode::ReadWrite
                                                                              : PosixArchiveFile::OpenMode::ReadOnly);
            if (recover) {
                journal = std::make_unique<PosixArchiveFile>(journal::journalPathFor(archivePath),
                                                             PosixArchiveFile::OpenMode::ReadWrite);
                recoverOnOpen(*archive, *journal);
            }
        } catch (const std::exception &e) {
            error = std::string("could not open the backup archive: ") + e.what();
            return false;
        }
        if (archive->size() == 0) {
            error = "there is no backup to compact";
            return false;
        }
        std::string openError;
        reader = Zip64Reader::tryOpen(*archive, &openError);
        if (!reader) {
            error = "the backup archive is unreadable: " + openError;
            return false;
        }
        std::optional<std::size_t> manifestIndex = reader->findEntry(ManifestEntryName);
        std::string manifestError;
        if (manifestIndex) {
            manifest = BackupManifest::parse(reader->readEntryToString(*manifestIndex), &manifestError);
        }
        if (!manifest) {
            error = "the backup archive's manifest is missing or damaged: " + manifestError;
            return false;
        }
        return true;
    }
};

std::uint64_t availableBytes(const fs::path &archivePath)
{
    std::error_code ec;
    fs::space_info info = fs::space(archivePath.parent_path(), ec);
    return ec ? 0 : info.available;
}

// Replace `archivePath` with `tempPath`. POSIX rename is atomic and just
// needs the directory flushed afterwards; on Windows a scanner or
// Explorer may hold the old file briefly, hence the retries.
bool replaceArchive(const fs::path &tempPath, const fs::path &archivePath, std::string &error)
{
#if defined(_WIN32)
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (MoveFileExW(tempPath.c_str(), archivePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        DWORD code = GetLastError();
        if (code != ERROR_SHARING_VIOLATION && code != ERROR_ACCESS_DENIED) {
            error = "could not replace the archive (error " + std::to_string(code) + ")";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200 * (attempt + 1)));
    }
    error = "could not replace the archive: another program keeps it open";
    return false;
#else
    std::error_code ec;
    fs::rename(tempPath, archivePath, ec);
    if (ec) {
        error = "could not replace the archive: " + ec.message();
        return false;
    }
    infrastructure::fsyncDirectoryContaining(archivePath.string());
    return true;
#endif
}

}  // namespace

fs::path CompactStickBackup::temporaryPathFor(const fs::path &archivePath)
{
    fs::path temp = archivePath;
    temp += ".compacting";
    return temp;
}

CompactionPreflight CompactStickBackup::preflight(const fs::path &archivePath, std::uint64_t freeSpaceMarginBytes)
{
    CompactionPreflight result;
    Opened opened;
    if (!opened.open(archivePath, false)) {
        result.error = opened.error;
        return result;
    }
    DeadSpaceReport report = deadSpace(*opened.reader);
    result.archiveBytes = report.fileSize;
    result.liveBytes = report.liveBytes + report.overheadBytes;
    result.deadBytes = report.deadBytes;
    result.deadRatio = report.deadRatio();
    result.suggested = shouldSuggestCompaction(report);
    result.requiredFreeBytes = result.liveBytes + freeSpaceMarginBytes;
    result.availableFreeBytes = availableBytes(archivePath);
    result.enoughFreeSpace = result.availableFreeBytes >= result.requiredFreeBytes;
    return result;
}

CompactionOutcome CompactStickBackup::execute(const CompactStickBackupOptions &options)
{
    CompactionOutcome outcome;
    // Held for the whole call, before the recovering open below: this
    // always recovers (unlike a restore's preview), so it must exclude a
    // concurrent BackupStick/RestoreStickBackup/another compaction on the
    // same archive from its very first touch of the file.
    std::unique_ptr<infrastructure::backup::StickWriteLock> lock;
    try {
        lock = std::make_unique<infrastructure::backup::StickWriteLock>(
            journal::lockPathFor(options.archivePath).string());
    } catch (const infrastructure::backup::StickBusyError &e) {
        outcome.message = e.what();
        return outcome;
    }
    const fs::path tempPath = temporaryPathFor(options.archivePath);
    std::error_code ec;
    fs::remove(tempPath, ec);  // a previous attempt that never finished

    Opened opened;
    if (!opened.open(options.archivePath, true)) {
        outcome.message = opened.error;
        return outcome;
    }
    DeadSpaceReport report = deadSpace(*opened.reader);
    outcome.bytesBefore = report.fileSize;
    outcome.requiredFreeBytes = report.liveBytes + report.overheadBytes + options.freeSpaceMarginBytes;
    outcome.availableFreeBytes = availableBytes(options.archivePath);
    if (report.deadBytes == 0) {
        outcome.status = CompactionOutcome::Status::NothingToReclaim;
        outcome.bytesAfter = report.fileSize;
        return outcome;
    }
    if (outcome.availableFreeBytes < outcome.requiredFreeBytes) {
        outcome.status = CompactionOutcome::Status::NotEnoughSpace;
        outcome.message = "compacting needs " + std::to_string(outcome.requiredFreeBytes) + " bytes free, "
                          + std::to_string(outcome.availableFreeBytes) + " available";
        return outcome;
    }

    try {
        {
            PosixArchiveFile temp(tempPath, PosixArchiveFile::OpenMode::ReadWrite);
            CompactionResult result = compactArchive(*opened.reader, *opened.manifest, temp, options.cancel, options.onProgress);
            if (result.cancelled) {
                outcome.status = CompactionOutcome::Status::Cancelled;
                outcome.bytesAfter = report.fileSize;
            } else {
                temp.barrier();
                std::string verifyError;
                if (!verifyArchiveTail(temp, 0, &verifyError)) {
                    throw ArchiveFormatError("the compacted copy did not verify: " + verifyError);
                }
                outcome.bytesAfter = temp.size();
            }
        }
        if (outcome.status == CompactionOutcome::Status::Cancelled) {
            fs::remove(tempPath, ec);
            return outcome;
        }
    } catch (const std::exception &e) {
        fs::remove(tempPath, ec);
        outcome.message = e.what();
        return outcome;
    }

    // Close our handles before the rename (Windows refuses to replace an
    // open file; POSIX does not care but there is no reason to keep them).
    opened.reader.reset();
    opened.archive.reset();
    opened.journal.reset();
    std::string replaceError;
    if (!replaceArchive(tempPath, options.archivePath, replaceError)) {
        fs::remove(tempPath, ec);
        outcome.message = replaceError;
        return outcome;
    }
    outcome.status = CompactionOutcome::Status::Compacted;
    return outcome;
}

}  // namespace seabass::application
