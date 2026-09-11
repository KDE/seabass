#include "application/use_cases/restore_stick_backup.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <system_error>

#include "infrastructure/backup/stick_write_lock.hpp"
#include "infrastructure/engine/engine_library_layout.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/stick_backup/archive_journal.hpp"
#include "infrastructure/stick_backup/archive_recovery.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/restore_path_sanitizer.hpp"
#include "infrastructure/stick_backup/sqlite_db_set.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"
#include "infrastructure/stick_backup/zip_format.hpp"

namespace seabass::application
{

namespace fs = std::filesystem;
using namespace infrastructure::stick_backup;
namespace engine = infrastructure::engine;

namespace
{

constexpr std::string_view TempSuffix = ".seabass-restore-tmp";

// A corrupted archive can still parse a perfectly plausible central
// directory -- names, sizes and mtimes are metadata the writer commits
// once, up front, and describe() / preview() never read a single content
// byte to confirm it. The only cheap, whole-archive check available
// before actually restoring is dataOffset(): it reads each entry's local
// header (tens of bytes, no file content) and confirms it is still where
// the central directory says it is. This is what caught a real archive
// where 19818 of 19828 entries -- everything except the last few files
// written -- had a well-formed central directory entry pointing at bytes
// that were, on disk, all zero: the restore ground through the whole
// plan file by file, created thousands of directories for content that
// was never coming, and only reported "9 files, but with problems" at
// the very end, past the point a person could still back out.
//
// Not a full CRC pass: that means reading the archive's entire content
// once (comparable cost to the restore itself), which is a much bigger
// ask to run before every preview. dataOffset() catches exactly the
// failure mode above -- a header that isn't there -- while staying cheap
// enough to run unconditionally.
std::size_t countUnreadableEntries(const Zip64Reader &reader)
{
    std::size_t unreadable = 0;
    for (std::size_t i = 0; i < reader.entries().size(); ++i) {
        try {
            reader.dataOffset(i);
        } catch (const ArchiveFormatError &) {
            ++unreadable;
        }
    }
    return unreadable;
}

struct Opened
{
    std::unique_ptr<PosixArchiveFile> archive;
    std::unique_ptr<PosixArchiveFile> journal;
    std::optional<Zip64Reader> reader;
    std::optional<BackupManifest> manifest;
    std::string error;
    std::size_t unreadableEntries = 0;

    // `recover`: apply a leftover journal (truncating the archive back to
    // its pre-update length) before reading. Only an operation the user
    // actually asked for on this one archive may do that.
    //
    // Listing calls must pass false. Recovery is a write, and a backup
    // that is *running* has a live journal by design -- so a listing pass
    // that recovered would truncate an archive mid-write while
    // BackupStick was still appending to it, leaving a file of the right
    // length holding nothing but its trailer. That is not theoretical:
    // BackupAdvisorController lists the backup folder on every stick
    // assessment, and it destroyed a real 12 GB backup this way. See
    // tests/backup_archive_concurrent_reader_test.cpp.
    bool open(const fs::path &archivePath, bool recover)
    {
        try {
            archive = std::make_unique<PosixArchiveFile>(
                archivePath, recover ? PosixArchiveFile::OpenMode::ReadWrite : PosixArchiveFile::OpenMode::ReadOnly);
            if (recover) {
                journal = std::make_unique<PosixArchiveFile>(journal::journalPathFor(archivePath),
                                                             PosixArchiveFile::OpenMode::ReadWrite);
                recoverOnOpen(*archive, *journal);
            }
        } catch (const std::exception &e) {
            error = std::string("could not open the backup: ") + e.what();
            return false;
        }
        if (archive->size() == 0) {
            error = "the backup file is empty";
            return false;
        }
        std::string openError;
        reader = Zip64Reader::tryOpen(*archive, &openError);
        if (!reader) {
            error = "the backup is unreadable: " + openError;
            return false;
        }
        std::optional<std::size_t> manifestIndex = reader->findEntry(ManifestEntryName);
        std::string manifestError;
        if (manifestIndex) {
            manifest = BackupManifest::parse(reader->readEntryToString(*manifestIndex), &manifestError);
        }
        if (!manifest) {
            error = "the backup's manifest is missing or damaged: " + manifestError;
            return false;
        }
        unreadableEntries = countUnreadableEntries(*reader);
        return true;
    }
};

struct PlannedEntry
{
    std::size_t index = 0;        // in reader.entries()
    std::string name;             // archive name
    fs::path relative;            // sanitized
    bool isDirectory = false;
    std::uint64_t size = 0;
    bool unchanged = false;       // file already on the target with the same size and mtime (and, for a database set, the same fingerprint)
    bool databaseMember = false;  // written last, sets kept together
    std::string setMainPath;      // databaseMember only: archive path of the set's main file
};

struct RestorePlan
{
    std::vector<PlannedEntry> directories;
    std::vector<PlannedEntry> files;  // non-DB first, then DB members grouped by set
    std::vector<std::pair<std::string, std::string>> rejected;
    std::size_t unchanged = 0;
    std::uint64_t bytesToWrite = 0;
    std::uint64_t totalBytes = 0;
    std::set<std::string> backupPaths;  // relative paths (files and dirs) the backup contains
};

RestorePlan planRestore(const Zip64Reader &reader, const BackupManifest &manifest, const fs::path &targetRoot)
{
    RestorePlan plan;
    std::vector<PlannedEntry> databaseFiles;
    std::map<std::string, const ManifestRow *> rowsByPath;
    for (const ManifestRow &row : manifest.rows) {
        rowsByPath.emplace(row.path, &row);
    }
    for (std::size_t i = 0; i < reader.entries().size(); ++i) {
        const CentralEntry &entry = reader.entries()[i];
        if (entry.name == ManifestEntryName) {
            continue;
        }
        std::string reason;
        bool isDirectory = false;
        std::optional<fs::path> relative = sanitizeEntryName(entry.name, hostTargetOs(), &reason, &isDirectory);
        if (!relative) {
            plan.rejected.emplace_back(entry.name, reason);
            continue;
        }
        PlannedEntry planned;
        planned.index = i;
        planned.name = entry.name;
        planned.relative = *relative;
        planned.isDirectory = isDirectory || entry.isDirectory;
        plan.backupPaths.insert(pathToUtf8(*relative));
        if (planned.isDirectory) {
            plan.directories.push_back(std::move(planned));
            continue;
        }
        plan.totalBytes += entry.size;
        planned.size = entry.size;
        std::error_code ec;
        fs::path target = targetRoot / *relative;
        if (fs::is_regular_file(target, ec)) {
            std::uint64_t size = fs::file_size(target, ec);
            std::int64_t mtime = ec ? 0 : toUnixSeconds(fs::last_write_time(target, ec));
            if (!ec && size == entry.size && std::llabs(mtime - entry.mtimeUnix) <= 2) {
                planned.unchanged = true;
            }
        }
        std::string filename = pathToUtf8(relative->filename());
        if (const std::optional<fs::path> mainFile = engine::dbSetMainFile(fs::path(filename))) {
            planned.databaseMember = true;
            planned.setMainPath = entry.name.substr(0, entry.name.size() - filename.size()) + pathToUtf8(*mainFile);
            // Size and mtime cannot tell a database apart from itself one
            // commit later (SQLite reuses pages; FAT keeps 2 s mtimes): a
            // set whose main file the manifest fingerprinted is unchanged
            // only when the target's fingerprint is the same.
            if (planned.unchanged && planned.setMainPath == entry.name) {
                auto row = rowsByPath.find(entry.name);
                if (row != rowsByPath.end() && !row->second->extra.empty()) {
                    const std::optional<DbSetFingerprint> live = fingerprintDbSet(target);
                    planned.unchanged = live && live->toHex() == row->second->extra;
                }
            }
        }
        (planned.databaseMember ? databaseFiles : plan.files).push_back(std::move(planned));
    }
    // A database set is written whole or not at all: any member that
    // changed (or a main file whose fingerprint moved) takes its
    // siblings with it.
    std::set<std::string> setsToWrite;
    for (const PlannedEntry &member : databaseFiles) {
        if (!member.unchanged) {
            setsToWrite.insert(member.setMainPath);
        }
    }
    for (PlannedEntry &member : databaseFiles) {
        if (setsToWrite.count(member.setMainPath) != 0) {
            member.unchanged = false;
        }
    }
    auto byName = [](const PlannedEntry &a, const PlannedEntry &b) { return a.name < b.name; };
    std::sort(plan.directories.begin(), plan.directories.end(), byName);
    std::sort(plan.files.begin(), plan.files.end(), byName);
    // Database members sort by name too, which keeps X.db, X.db-journal,
    // X.db-wal adjacent -- one set is written without other files between.
    std::sort(databaseFiles.begin(), databaseFiles.end(), byName);
    plan.files.insert(plan.files.end(), databaseFiles.begin(), databaseFiles.end());
    for (const PlannedEntry &file : plan.files) {
        if (file.unchanged) {
            ++plan.unchanged;
        } else {
            plan.bytesToWrite += file.size;
        }
    }
    return plan;
}

std::vector<std::string> extrasOnTarget(const fs::path &targetRoot, const std::set<std::string> &backupPaths)
{
    std::vector<std::string> extras;
    TreeWalk walk = walkStickTree(targetRoot, CancellationToken::none());
    for (const TreeEntry &entry : walk.entries) {
        if (backupPaths.count(entry.relativePath) == 0) {
            extras.push_back(entry.relativePath);
        }
    }
    return extras;
}

std::uint64_t availableBytes(const fs::path &root)
{
    std::error_code ec;
    fs::space_info info = fs::space(root, ec);
    return ec ? 0 : info.available;
}

// Streams one entry to `destination` via a temporary sibling, verifying
// both the CRC from the central directory and the SHA-256 from the
// manifest on what was read, flushing, then renaming into place and
// restoring the mtime. Returns an error message or empty.
std::string writeEntry(const Zip64Reader &reader, const PlannedEntry &planned, const ManifestRow *row,
                       const fs::path &destination, std::size_t chunkSize, const std::function<void(std::uint64_t)> &progress)
{
    const CentralEntry &entry = reader.entries()[planned.index];
    if (row == nullptr) {
        return entry.name + " is not described by the backup's manifest; not restored";
    }
    fs::path temp = destination;
    temp += std::string(TempSuffix);
    std::error_code ec;
    fs::remove(longPathSafe(temp), ec);
    try {
        {
            PosixArchiveFile out(longPathSafe(temp), PosixArchiveFile::OpenMode::ReadWrite);
            // The temp file belongs to this function alone and must start
            // empty, but ReadWrite opens without truncating (OPEN_ALWAYS /
            // O_CREAT, which ArchiveUpdater depends on to resume an existing
            // archive). Truncating here is what makes the entry correct,
            // rather than the remove above having worked -- and that remove
            // cannot be relied on: on Windows a destination past MAX_PATH
            // makes the unprefixed fs::remove fail while *reporting success*
            // in its error_code, so a leftover temp survives and append()
            // starts at its end, writing the entry after the stale bytes.
            //
            // Nothing downstream would catch that. The CRC, the SHA-256 and
            // the byte count are all computed over the bytes read out of the
            // archive, never over the file on disk, so all three still match,
            // the corrupt file is renamed into place, and the restore reports
            // Restored with no warning at all.
            out.truncate(0);
            std::uint32_t crc = 0;
            infrastructure::hashing::Sha256 hasher;
            std::uint64_t written = 0;
            reader.readEntry(
                planned.index,
                [&](std::span<const std::byte> piece) {
                    out.append(piece);
                    crc = static_cast<std::uint32_t>(
                        ::crc32(crc, reinterpret_cast<const Bytef *>(piece.data()), static_cast<uInt>(piece.size())));
                    hasher.update(piece);
                    written += piece.size();
                    if (progress) {
                        progress(written);
                    }
                },
                chunkSize);
            if (crc != entry.crc32 || written != entry.size || hasher.finish() != row->sha256) {
                fs::remove(longPathSafe(temp), ec);
                return "backup data for " + entry.name + " is damaged (checksum mismatch); not restored";
            }
            out.barrier();
        }
        fs::last_write_time(longPathSafe(temp), fromUnixSeconds(entry.mtimeUnix), ec);
        fs::rename(longPathSafe(temp), longPathSafe(destination), ec);
        if (ec) {
            fs::remove(longPathSafe(temp), ec);
            return "could not place " + entry.name + ": " + ec.message();
        }
    } catch (const std::exception &e) {
        fs::remove(longPathSafe(temp), ec);
        return "could not write " + entry.name + ": " + e.what();
    }
    return {};
}

}  // namespace

StickBackupDescription RestoreStickBackup::describe(const fs::path &archivePath)
{
    StickBackupDescription description;
    description.archivePath = archivePath;
    Opened opened;
    // Listing only: never recovers, so this is safe to run against a
    // folder someone is backing up into right now.
    if (!opened.open(archivePath, false)) {
        description.error = opened.error;
        return description;
    }
    description.stickLabel = opened.manifest->stickLabel;
    description.stickIdentifier = opened.manifest->stickIdentifier;
    description.status = opened.manifest->status;
    description.createdAtUnix = opened.manifest->createdAtUnix;
    description.entries = opened.manifest->rows.size();
    description.archiveBytes = opened.archive->size();
    description.libraryFingerprint = opened.manifest->libraryFingerprint;
    for (const ManifestRow &row : opened.manifest->rows) {
        if (!row.extra.empty()) {
            description.databaseFingerprints.emplace_back(row.path, row.extra);
        }
    }
    return description;
}

std::vector<StickBackupDescription> RestoreStickBackup::describeAll(const fs::path &directory)
{
    std::vector<StickBackupDescription> descriptions;
    std::error_code ec;
    for (const fs::directory_entry &entry : fs::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".zip") {
            continue;
        }
        descriptions.push_back(describe(entry.path()));
    }
    std::stable_sort(descriptions.begin(), descriptions.end(),
                     [](const StickBackupDescription &a, const StickBackupDescription &b) {
                         if (a.error.empty() != b.error.empty()) {
                             return a.error.empty();
                         }
                         return a.createdAtUnix > b.createdAtUnix;
                     });
    return descriptions;
}

int RestoreStickBackup::countArchives(const fs::path &directory)
{
    int count = 0;
    std::error_code ec;
    for (const fs::directory_entry &entry : fs::directory_iterator(directory, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".zip") {
            count++;
        }
    }
    return count;
}

RestorePreview RestoreStickBackup::preview(const RestoreOptions &options)
{
    RestorePreview preview;
    Opened opened;
    if (!opened.open(options.archivePath, true)) {
        preview.error = opened.error;
        return preview;
    }
    preview.stickLabel = opened.manifest->stickLabel;
    preview.stickIdentifier = opened.manifest->stickIdentifier;
    preview.status = opened.manifest->status;
    preview.createdAtUnix = opened.manifest->createdAtUnix;
    preview.unreadableEntries = opened.unreadableEntries;

    RestorePlan plan = planRestore(*opened.reader, *opened.manifest, options.targetRoot);
    preview.entries = plan.directories.size() + plan.files.size();
    preview.bytes = plan.totalBytes;
    preview.filesUnchanged = plan.unchanged;
    preview.filesToWrite = plan.files.size() - plan.unchanged;
    preview.bytesToWrite = plan.bytesToWrite;
    preview.rejected = plan.rejected;
    std::error_code ec;
    if (fs::is_directory(options.targetRoot, ec)) {
        preview.extras = extrasOnTarget(options.targetRoot, plan.backupPaths).size();
        preview.targetHasEngineLibrary = fs::exists(engine::engineMainDatabasePath(options.targetRoot), ec);
        preview.freeBytesAtTarget = availableBytes(options.targetRoot);
        preview.enoughFreeSpace = preview.freeBytesAtTarget >= plan.bytesToWrite + options.freeSpaceMarginBytes;
    }
    return preview;
}

RestoreSummary RestoreStickBackup::execute(const RestoreOptions &options, ProgressReporter &reporter)
{
    RestoreSummary summary;
    // Held for the whole call, before the recovering open below: recovery
    // truncates an in-flight archive, so this must exclude a concurrent
    // BackupStick/CompactStickBackup/another restore on the same archive,
    // not just the restore's own writes.
    std::unique_ptr<infrastructure::backup::StickWriteLock> lock;
    try {
        lock = std::make_unique<infrastructure::backup::StickWriteLock>(
            journal::lockPathFor(options.archivePath).string());
    } catch (const infrastructure::backup::StickBusyError &e) {
        summary.message = e.what();
        return summary;
    }
    Opened opened;
    if (!opened.open(options.archivePath, true)) {
        summary.message = opened.error;
        return summary;
    }
    // Half is a deliberately blunt line. A handful of unreadable entries
    // among thousands is already handled per-file below (one line in
    // writeErrors, everything else still restores); this is for the other
    // case, where the archive itself is not usable and every one of those
    // per-file attempts is doomed before it starts. Refusing here is what
    // stands between that and grinding through the whole plan file by
    // file, creating a directory for every one of them, to land on
    // "Restored 9 files, but with problems" as the first hint of the
    // real scale of it -- past the point of taking the target stick back.
    if (const std::size_t total = opened.reader->entries().size(); total > 0 && opened.unreadableEntries * 2 > total) {
        summary.message = std::to_string(opened.unreadableEntries) + " of " + std::to_string(total)
                          + " entries in this backup have no readable data -- the archive appears to be damaged; nothing was restored";
        return summary;
    }
    std::error_code ec;
    if (!fs::is_directory(options.targetRoot, ec)) {
        summary.message = "the restore target is not a directory: " + options.targetRoot.string();
        return summary;
    }

    RestoreProgress progress;
    auto report = [&](RestoreProgress::Phase phase) {
        progress.phase = phase;
        if (options.onProgress) {
            options.onProgress(progress);
        }
    };
    report(RestoreProgress::Phase::Analyzing);
    RestorePlan plan = planRestore(*opened.reader, *opened.manifest, options.targetRoot);
    summary.rejected = plan.rejected;
    summary.filesUnchanged = plan.unchanged;
    std::vector<std::string> extras = options.exact ? extrasOnTarget(options.targetRoot, plan.backupPaths) : std::vector<std::string>{};

    std::uint64_t freeBytes = availableBytes(options.targetRoot);
    if (freeBytes < plan.bytesToWrite + options.freeSpaceMarginBytes) {
        summary.message = "not enough free space on the target: needs " + std::to_string(plan.bytesToWrite + options.freeSpaceMarginBytes)
                          + " bytes, " + std::to_string(freeBytes) + " available";
        return summary;
    }

    for (const PlannedEntry &dir : plan.directories) {
        fs::path target = options.targetRoot / dir.relative;
        if (!fs::is_directory(target, ec)) {
            if (fs::create_directories(longPathSafe(target), ec) && !ec) {
                ++summary.directoriesCreated;
            } else if (ec) {
                summary.writeErrors.push_back(dir.name + ": " + ec.message());
                ec.clear();
            }
        }
    }

    std::map<std::string, const ManifestRow *> rowsByPath;
    for (const ManifestRow &row : opened.manifest->rows) {
        rowsByPath.emplace(row.path, &row);
    }

    progress.filesTotal = plan.files.size() - plan.unchanged;
    progress.bytesTotal = plan.bytesToWrite;
    report(RestoreProgress::Phase::Writing);
    reporter.start("Restoring files", progress.filesTotal);
    for (const PlannedEntry &file : plan.files) {
        if (options.cancel.cancelled()) {
            reporter.finish();
            summary.status = RestoreSummary::Status::Cancelled;
            summary.message = "cancelled; files already restored are complete, nothing half-written was left behind";
            return summary;
        }
        if (file.unchanged) {
            continue;
        }
        progress.currentFile = file.name;
        fs::path destination = options.targetRoot / file.relative;
        fs::create_directories(longPathSafe(destination.parent_path()), ec);
        ec.clear();
        std::uint64_t bytesBefore = progress.bytesDone;
        auto rowIt = rowsByPath.find(file.name);
        const ManifestRow *row = rowIt == rowsByPath.end() ? nullptr : rowIt->second;
        std::string error = writeEntry(*opened.reader, file, row, destination, options.chunkSize, [&](std::uint64_t bytes) {
            progress.bytesDone = bytesBefore + bytes;
            report(RestoreProgress::Phase::Writing);
        });
        if (!error.empty()) {
            summary.writeErrors.push_back(error);
            progress.bytesDone = bytesBefore;
            // A yanked stick would otherwise produce one error per
            // remaining file: stop at the first error whose target is gone.
            if (!fs::is_directory(options.targetRoot, ec)) {
                ec.clear();
                reporter.finish();
                summary.status = RestoreSummary::Status::Failed;
                summary.message = "the drive disappeared after " + std::to_string(summary.filesWritten)
                                  + " files were restored; the files already restored are complete. Reconnect it and "
                                    "restore again to continue where this left off.";
                return summary;
            }
        } else {
            ++summary.filesWritten;
            summary.bytesWritten += opened.reader->entries()[file.index].size;
            progress.bytesDone = bytesBefore + opened.reader->entries()[file.index].size;
        }
        progress.filesDone = summary.filesWritten + summary.writeErrors.size();
        reporter.tick(progress.filesDone);
        report(RestoreProgress::Phase::Writing);
    }
    reporter.finish();

    if (options.exact && !extras.empty()) {
        report(RestoreProgress::Phase::Removing);
        // Files first, then directories deepest-first so they are empty.
        std::sort(extras.begin(), extras.end(), [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
        for (const std::string &extra : extras) {
            fs::path target = options.targetRoot / pathFromUtf8(extra);
            if (fs::is_directory(target, ec)) {
                fs::remove(target, ec);  // only if empty by now
            } else {
                fs::remove(target, ec);
            }
            if (ec) {
                summary.warnings.push_back(extra + ": could not remove: " + ec.message());
                ec.clear();
            } else {
                ++summary.extrasRemoved;
            }
        }
    }

    if (options.libraryCheck) {
        report(RestoreProgress::Phase::Checking);
        try {
            summary.missingTrackPaths = options.libraryCheck(options.targetRoot);
        } catch (const std::exception &e) {
            summary.warnings.push_back(std::string("the restored database could not be checked: ") + e.what());
        }
    }

    bool problems = !summary.rejected.empty() || !summary.writeErrors.empty()
                    || (summary.missingTrackPaths && !summary.missingTrackPaths->empty()) || !summary.warnings.empty();
    summary.status = problems ? RestoreSummary::Status::RestoredWithProblems : RestoreSummary::Status::Restored;
    return summary;
}

}  // namespace seabass::application
