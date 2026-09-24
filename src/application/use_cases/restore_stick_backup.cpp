// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/restore_stick_backup.hpp"

#include "infrastructure/backup/stick_space.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/fs_remove.hpp"

#include <zlib.h>

#include <algorithm>
#include <string>
#include <fstream>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <system_error>

#include "application/path_key.hpp"
#include "infrastructure/backup/stick_write_lock.hpp"
#include "infrastructure/engine/engine_library_layout.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/stick_backup/archive_journal.hpp"
#include "infrastructure/stick_backup/archive_recovery.hpp"
#include "infrastructure/stick_backup/archive_updater.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/restore_path_sanitizer.hpp"
#include "infrastructure/stick_backup/sqlite_db_set.hpp"
#include "infrastructure/stick_backup/stat_diff.hpp"
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

// A read-only view of an archive's first `length` bytes: the generation an
// unfinished update started from, which is exactly what rolling that update
// back will leave. Lets a preview read it without truncating anything.
class SettledArchiveView final : public ArchiveFile
{
public:
    SettledArchiveView(const ArchiveFile &file, std::uint64_t length) : m_file(file), m_length(length) {}
    std::uint64_t size() const override { return m_length; }
    void readAt(std::uint64_t offset, std::span<std::byte> out) const override
    {
        if (offset > m_length || out.size() > m_length - offset) {
            throw ArchiveIoError("read past the settled end of the backup");
        }
        m_file.readAt(offset, out);
    }
    void append(std::span<const std::byte>) override { throw ArchiveIoError("a settled view of a backup is read-only"); }
    void truncate(std::uint64_t) override { throw ArchiveIoError("a settled view of a backup is read-only"); }
    void barrier() override { throw ArchiveIoError("a settled view of a backup is read-only"); }

private:
    const ArchiveFile &m_file;
    std::uint64_t m_length;
};

struct Opened
{
    std::unique_ptr<PosixArchiveFile> archive;
    std::unique_ptr<PosixArchiveFile> journal;
    // Preview of an unfinished update: what the rollback will leave. Declared
    // before `reader`, which reads through it and must go first.
    std::unique_ptr<ArchiveFile> settled;
    bool rollsBackUnfinishedUpdate = false;
    std::optional<Zip64Reader> reader;
    std::optional<BackupManifest> manifest;
    std::string error;
    std::size_t unreadableEntries = 0;

    enum class Journal
    {
        // Listing: read-only, and cheap, because the backup list is read over
        // and over while a backup runs. An archive that does not open from
        // its end is read as the generation its last update started from.
        List,
        // Preview of one archive: read-only, but checks everything an update
        // appended, so its figures are exactly what the restore will write.
        Preview,
        // The restore itself: with the lock, hand the journal to
        // recoverOnOpen, which rolls an unfinished update back or clears a
        // journal that outlived a finished one. Without the lock, or when
        // the archive cannot be opened for writing, read like Preview.
        Recover,
    };

    // Set by execute(): whether it holds the archive's write lock. A folder
    // nobody can write to cannot hold the lock file -- nor a backup writing
    // into it -- so the restore still reads the archive (its settled
    // generation, if an update did not finish) but rolls nothing back.
    bool canWrite = true;

    const ArchiveFile &readable() const { return settled ? *settled : *archive; }

    // The journal record of an update that did not finish, if there is one.
    // Read-only: looks at the journal, never clears it. `thorough` checks the
    // bytes an update appended the way the rollback does -- an archive can
    // open from its end and still be unfinished, when an update wrote its
    // central directory and then failed its own verification.
    std::optional<JournalRecord> unfinishedUpdate(const fs::path &archivePath, bool thorough) const
    {
        const fs::path journalPath = journal::journalPathFor(archivePath);
        std::error_code ec;
        if (!fs::exists(journalPath, ec) || fs::file_size(journalPath, ec) == 0) {
            return std::nullopt;
        }
        const JournalState state = journal::read(PosixArchiveFile(journalPath, PosixArchiveFile::OpenMode::ReadOnly));
        if (state.kind != JournalState::Kind::Valid) {
            return std::nullopt;  // absent, or corrupt: nothing was appended
        }
        if (archive->size() < state.record.preLength) {
            return state.record;
        }
        // Listing settles for "opens from its end": a thorough check of a
        // large first backup's journal would re-read the whole archive on
        // every pass of the list. The rare update that wrote its central
        // directory and then failed verification is listed as it opens, and
        // its preview -- thorough -- shows the rollback.
        const bool finished = thorough ? verifyArchiveTail(*archive, state.record.preLength)
                                       : Zip64Reader::tryOpen(*archive).has_value();
        if (finished) {
            return std::nullopt;  // the update finished; only its journal outlived it
        }
        return state.record;
    }

    void readSettled(const JournalRecord &record)
    {
        if (archive->size() < record.preLength) {
            throw ArchiveIoError("the backup is shorter than its journaled pre-update length");
        }
        settled = std::make_unique<SettledArchiveView>(*archive, record.preLength);
        rollsBackUnfinishedUpdate = true;
    }

    // Opens read-only whenever it can. Preview used to open read-write and
    // recover like execute(): it could not read a write-protected backup at
    // all, and it truncated an archive a running backup was still appending
    // to -- the damage BackupAdvisorController once did to a real 12 GB
    // backup through describe(). See
    // tests/backup_archive_concurrent_reader_test.cpp.
    bool open(const fs::path &archivePath, Journal mode)
    {
        try {
            archive = std::make_unique<PosixArchiveFile>(archivePath, PosixArchiveFile::OpenMode::ReadOnly);
            const fs::path journalPath = journal::journalPathFor(archivePath);
            std::error_code ec;
            const bool journalPresent = fs::exists(journalPath, ec) && fs::file_size(journalPath, ec) > 0;
            bool recovered = false;
            if (mode == Journal::Recover && canWrite && journalPresent) {
                std::unique_ptr<PosixArchiveFile> writableArchive;
                std::unique_ptr<PosixArchiveFile> writableJournal;
                try {
                    writableArchive = std::make_unique<PosixArchiveFile>(archivePath, PosixArchiveFile::OpenMode::ReadWrite);
                    writableJournal = std::make_unique<PosixArchiveFile>(journalPath, PosixArchiveFile::OpenMode::ReadWrite);
                } catch (const std::exception &) {
                    // A write-protected archive (or journal) in a writable
                    // folder: roll nothing back, read what is settled.
                    canWrite = false;
                }
                if (writableArchive && writableJournal) {
                    archive = std::move(writableArchive);
                    journal = std::move(writableJournal);
                    // One pass: rolls an unfinished update back, or clears a
                    // journal that outlived a finished one (or a corrupt one).
                    recoverOnOpen(*archive, *journal);
                    recovered = true;
                }
            }
            if (!recovered && journalPresent) {
                if (std::optional<JournalRecord> record = unfinishedUpdate(archivePath, mode != Journal::List)) {
                    readSettled(*record);
                }
            }
        } catch (const std::exception &e) {
            error = std::string("could not open the backup: ") + e.what();
            return false;
        }
        if (readable().size() == 0) {
            error = "the backup file is empty";
            return false;
        }
        std::string openError;
        reader = Zip64Reader::tryOpen(readable(), &openError);
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
    std::set<std::string> backupDirectories;  // the directories among them
    // The files among them, by size: the candidates an extra could be the same
    // file as (see extraIsBackupFile).
    std::multimap<std::uint64_t, fs::path> backupFilesBySize;
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
        if (infrastructure::stick_backup::isArchiveMetadataEntry(entry.name)) {
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
            plan.backupDirectories.insert(pathToUtf8(*relative));
            plan.directories.push_back(std::move(planned));
            continue;
        }
        plan.backupFilesBySize.emplace(entry.size, *relative);
        plan.totalBytes += entry.size;
        planned.size = entry.size;
        std::error_code ec;
        fs::path target = targetRoot / *relative;
        if (fs::is_regular_file(target, ec)) {
            std::uint64_t size = fs::file_size(target, ec);
            std::int64_t mtime = ec ? 0 : toUnixSeconds(fs::last_write_time(target, ec));
            if (!ec && size == entry.size
                && infrastructure::stick_backup::mtimeMatchesRecorded(entry.mtimeUnix, mtime)) {
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

// What exact mode removes: everything on the target the backup does not hold.
//
// Compared by normalizedPathKey, not byte for byte, and only file with file,
// directory with directory. DJ sticks are exFAT or FAT32, where
// "Contents/ARTBAT" and "Contents/Artbat" are one folder: a byte-exact list
// called the stick's spelling of a file the restore had just written an extra
// and deleted it. The folding is an approximation of what a filesystem treats
// as one name, though -- extraIsBackupFile asks the filesystem itself before
// anything is removed.
//
// The folders holding the stick's own write lock are not extras either: the
// walk skips the lock file itself, so they are never empty and removing them
// could only fail.
std::vector<std::string> extrasOnTarget(const fs::path &targetRoot, const RestorePlan &plan)
{
    // Key -> the backup's own spelling, so a match that is only a match
    // after folding can be put to the filesystem (below).
    std::map<std::string, std::string> fileKeys;
    std::map<std::string, std::string> directoryKeys;
    for (const std::string &path : plan.backupPaths) {
        (plan.backupDirectories.count(path) != 0 ? directoryKeys : fileKeys).emplace(normalizedPathKey(path), path);
    }
    const std::string lockPath = "Seabass/backups/.write.lock";
    std::vector<std::string> extras;
    TreeWalk walk = walkStickTree(targetRoot, CancellationToken::none());
    for (const TreeEntry &entry : walk.entries) {
        const std::string &path = entry.relativePath;
        const std::map<std::string, std::string> &keys = entry.isDirectory ? directoryKeys : fileKeys;
        if (const auto match = keys.find(normalizedPathKey(path)); match != keys.end()) {
            // The backup's spelling, or another the filesystem resolves to
            // the same file: on exFAT "CONTENTS/B.MP3" is the backup's
            // "Contents/b.mp3". On a case-sensitive filesystem the two are
            // separate files and the variant is a stale extra; skipping it
            // on the key alone made an exact restore there not exact.
            std::error_code ec;
            if (match->second == path
                || (fs::equivalent(longPathSafe(targetRoot / pathFromUtf8(path)),
                                   longPathSafe(targetRoot / pathFromUtf8(match->second)), ec)
                    && !ec)) {
                continue;
            }
        }
        if (entry.isDirectory && lockPath.compare(0, path.size() + 1, path + "/") == 0) {
            continue;
        }
        extras.push_back(path);
    }
    return extras;
}

// Whether the extra at `extraPath` is, on this filesystem, the very same file
// as one the backup holds -- a name the filesystem folds together that
// normalizedPathKey does not (letters beyond its tables, Greek final sigma,
// NFC against NFD on macOS). Asked just before removal, after the restore has
// written: removing it would remove the restored file.
bool extraIsBackupFile(const fs::path &targetRoot, const fs::path &extraPath, const RestorePlan &plan)
{
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(longPathSafe(extraPath), ec);
    if (ec) {
        return false;
    }
    const auto [first, last] = plan.backupFilesBySize.equal_range(static_cast<std::uint64_t>(size));
    for (auto it = first; it != last; ++it) {
        if (fs::equivalent(longPathSafe(extraPath), longPathSafe(targetRoot / it->second), ec) && !ec) {
            return true;
        }
        ec.clear();
    }
    return false;
}

using infrastructure::backup::availableBytes;

// Streams one entry to `destination` via a temporary sibling, verifying
// both the CRC from the central directory and the SHA-256 from the
// manifest on what was read, flushing, then renaming into place and
// restoring the mtime. Returns an error message or empty.
std::string writeEntry(const Zip64Reader &reader, const PlannedEntry &planned, const ManifestRow *row,
                       const fs::path &destination, std::size_t chunkSize,
                       const std::function<void(std::uint64_t)> &progress, std::set<fs::path> *placedIn)
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
        if (placedIn != nullptr) {
            placedIn->insert(destination.parent_path());
        }
    } catch (const std::exception &e) {
        fs::remove(longPathSafe(temp), ec);
        return "could not write " + entry.name + ": " + e.what();
    }
    return {};
}

// Whether a new file can be created in `folder`: the only portable test,
// since permission bits say nothing on Windows or on a read-only mount.
// Creates and removes one uniquely named empty file; only asked after the
// lock file itself could not be created there.
bool folderIsWritable(const fs::path &folder)
{
    const fs::path probe = folder / (".seabass-write-probe-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        std::ofstream out(probe, std::ios::binary);
        if (!out.is_open()) {
            return false;
        }
    }
    std::error_code ec;
    fs::remove(probe, ec);
    return true;
}

}  // namespace

StickBackupDescription RestoreStickBackup::describe(const fs::path &archivePath)
{
    StickBackupDescription description;
    description.archivePath = archivePath;
    Opened opened;
    // Listing only: read-only, and an unfinished update is listed as the
    // backup it will roll back to rather than as unreadable -- otherwise the
    // Restore page could not offer it. Safe to run against a folder someone
    // is backing up into right now.
    if (!opened.open(archivePath, Opened::Journal::List)) {
        description.error = opened.error;
        return description;
    }
    description.stickLabel = opened.manifest->stickLabel;
    description.stickIdentifier = opened.manifest->stickIdentifier;
    description.status = opened.manifest->status;
    description.createdAtUnix = opened.manifest->createdAtUnix;
    description.entries = opened.manifest->rows.size();
    // The settled size for an unfinished update: what a restore would read.
    description.archiveBytes = opened.readable().size();
    description.libraryFingerprint = opened.manifest->libraryFingerprint;
    description.sourceReadOnly = opened.manifest->sourceReadOnly;
    description.userName = opened.manifest->userName;
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
    if (!opened.open(options.archivePath, Opened::Journal::Preview)) {
        preview.error = opened.error;
        return preview;
    }
    preview.stickLabel = opened.manifest->stickLabel;
    preview.stickIdentifier = opened.manifest->stickIdentifier;
    preview.status = opened.manifest->status;
    preview.sourceReadOnly = opened.manifest->sourceReadOnly;
    preview.createdAtUnix = opened.manifest->createdAtUnix;
    preview.unreadableEntries = opened.unreadableEntries;
    preview.rollsBackUnfinishedUpdate = opened.rollsBackUnfinishedUpdate;

    RestorePlan plan = planRestore(*opened.reader, *opened.manifest, options.targetRoot);
    preview.entries = plan.directories.size() + plan.files.size();
    preview.bytes = plan.totalBytes;
    preview.filesUnchanged = plan.unchanged;
    preview.filesToWrite = plan.files.size() - plan.unchanged;
    preview.bytesToWrite = plan.bytesToWrite;
    preview.rejected = plan.rejected;
    std::error_code ec;
    if (fs::is_directory(options.targetRoot, ec)) {
        preview.extras = extrasOnTarget(options.targetRoot, plan).size();
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
    Opened opened;
    try {
        lock = std::make_unique<infrastructure::backup::StickWriteLock>(
            journal::lockPathFor(options.archivePath).string());
    } catch (const infrastructure::backup::StickBusyError &e) {
        summary.message = e.what();
        return summary;
    } catch (const std::exception &e) {
        // Only a folder nobody can write to (a read-only share, a
        // write-protected folder) may do without the lock: nothing can be
        // writing the archive there either. Any other failure -- a full
        // disk, a lock file another account owns -- is reported, not
        // quietly run unlocked beside a backup that may hold the lock.
        if (folderIsWritable(options.archivePath.parent_path())) {
            summary.message = std::string("could not lock the backup: ") + e.what();
            return summary;
        }
        opened.canWrite = false;
    }
    if (!opened.open(options.archivePath, Opened::Journal::Recover)) {
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
    std::vector<std::string> extras = options.exact ? extrasOnTarget(options.targetRoot, plan) : std::vector<std::string>{};

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

    // A database set is restored whole or not at all, and that includes
    // a set the backup holds only part of. When the target already has a
    // whole copy of a member the backup could only salvage in part, the
    // good copy stays -- see below -- and so must its siblings: a -wal
    // written from the backup beside the target's own main file is a
    // database made of two different moments.
    std::set<std::string> setsHeldOnTarget;
    for (const PlannedEntry &file : plan.files) {
        auto rowIt = rowsByPath.find(file.name);
        if (!file.databaseMember || file.unchanged || rowIt == rowsByPath.end() || rowIt->second->salvagedFromSize == 0) {
            continue;
        }
        std::error_code existsEc;
        const std::uint64_t onTarget = fs::file_size(longPathSafe(options.targetRoot / file.relative), existsEc);
        if (!existsEc && onTarget >= rowIt->second->salvagedFromSize) {
            setsHeldOnTarget.insert(file.setMainPath);
        }
    }

    // And an exact restore must not then remove the set's other members
    // from the target as extras. The backup may hold only the main file
    // -- a -wal the failing stick would not open is simply not in it --
    // and the target's own -wal or -journal is part of the copy being
    // kept: removed, the kept database loses the commits in it, or is
    // left mid-transaction.
    if (!setsHeldOnTarget.empty()) {
        std::set<std::string> heldKeys;
        for (const std::string &main : setsHeldOnTarget) {
            heldKeys.insert(normalizedPathKey(main));
        }
        extras.erase(std::remove_if(extras.begin(), extras.end(),
                                    [&](const std::string &extra) {
                                        // Stick-relative, forward slashes, UTF-8: split
                                        // as a string, not through fs::path.
                                        const std::size_t slash = extra.rfind('/');
                                        const std::string dir =
                                            slash == std::string::npos ? std::string() : extra.substr(0, slash + 1);
                                        const auto main = engine::dbSetMainFile(
                                            pathFromUtf8(extra.substr(dir.size())));
                                        if (!main) {
                                            return false;
                                        }
                                        const std::string mainPath = dir + pathToUtf8(*main);
                                        return heldKeys.count(normalizedPathKey(mainPath)) != 0;
                                    }),
                     extras.end());
    }

    // Every directory a file was renamed into, fsynced once before this
    // reports anything as complete.
    //
    // writeEntry() does its own rename, and fsyncDirectoryContaining() is
    // the second half of that pattern -- its header says so and names
    // callers that do their own rename as the reason it is exposed.
    // Without it the directory entry can sit in write-back on POSIX, so
    // a stick pulled straight after a restore comes back with files
    // missing or zero length, while the cancel and drive-gone paths both
    // tell the user the files already restored are complete.
    //
    // Once per directory rather than once per file: the guarantee is the
    // same and a restore of ten thousand tracks does not pay ten
    // thousand fsyncs.
    // Paths, not strings: path::string() narrows through the ANSI code
    // page on Windows and throws for a folder name outside it.
    std::set<fs::path> placedIn;
    auto flushDirectories = [&placedIn]() {
        for (const fs::path &dir : placedIn) {
            infrastructure::fsyncDirectoryContaining(infrastructure::stick_backup::pathToUtf8(dir / "x"));
        }
        placedIn.clear();
    };

    progress.filesTotal = plan.files.size() - plan.unchanged;
    progress.bytesTotal = plan.bytesToWrite;
    report(RestoreProgress::Phase::Writing);
    reporter.start("Restoring files", progress.filesTotal);
    for (const PlannedEntry &file : plan.files) {
        if (options.cancel.cancelled()) {
            reporter.finish();
            flushDirectories();
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
        // A file the backup only holds part of, because the stick it came
        // off had stopped giving bytes. Writing it is usually right --
        // on a fresh stick it is all there is, and part of a track beats
        // none of it -- but not over a copy that is whole. Four megabytes
        // of a nine megabyte track written silently over the nine is the
        // one outcome a salvage backup must never cause, and a restore
        // that says "Restored" afterwards is how it would happen.
        if (file.databaseMember && setsHeldOnTarget.count(file.setMainPath) != 0) {
            if (row != nullptr && row->salvagedFromSize != 0) {
                summary.partial.push_back({file.name, row->size, row->salvagedFromSize, false});
            } else {
                // A member the backup holds WHOLE, kept off the drive
                // anyway because a sibling of it is only a part and the
                // drive's own copy of that sibling is whole. Holding the
                // set is right, staying quiet about it is not: the file
                // is not in `partial` (nothing about it is partial), so
                // without this the run reported the set's main database
                // only inside filesUnchanged, and a restore that did not
                // write the database said "Restored".
                summary.warnings.push_back(file.name
                                           + ": not written, because only part of this database's "
                                             "other files could be read off the failing drive and the copy already "
                                             "here is whole. The database on this drive was left exactly as it is.");
            }
            // Neither arm wrote anything, and neither is "unchanged":
            // one is a part the drive already beats, the other a whole
            // file withheld to keep the set from being assembled out of
            // two moments. Counted apart from filesUnchanged so the
            // number the page reads aloud as "already up to date" is
            // only ever about files that really are.
            ++summary.filesHeldBack;
            continue;
        }
        if (row != nullptr && row->salvagedFromSize != 0) {
            std::error_code existsEc;
            const std::uint64_t onTarget = fs::file_size(longPathSafe(destination), existsEc);
            const bool wholeCopyIsThere = !existsEc && onTarget >= row->salvagedFromSize;
            summary.partial.push_back({file.name, row->size, row->salvagedFromSize, !wholeCopyIsThere});
            if (wholeCopyIsThere) {
                // Counted as unchanged rather than written: nothing was
                // put on the target for this entry, and the file that is
                // there is the better one.
                ++summary.filesUnchanged;
                continue;
            }
        }
        std::string error = writeEntry(
            *opened.reader, file, row, destination, options.chunkSize,
            [&](std::uint64_t bytes) {
                progress.bytesDone = bytesBefore + bytes;
                report(RestoreProgress::Phase::Writing);
            },
            &placedIn);
        if (!error.empty()) {
            summary.writeErrors.push_back(error);
            progress.bytesDone = bytesBefore;
            // A yanked stick would otherwise produce one error per
            // remaining file: stop at the first error whose target is gone.
            if (!fs::is_directory(options.targetRoot, ec)) {
                ec.clear();
                reporter.finish();
                flushDirectories();
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
    // Before anything below can remove an extra or report a result: the
    // renames above are not durable until their directories are.
    flushDirectories();

    if (options.exact && !extras.empty() && (!summary.writeErrors.empty() || !summary.rejected.empty())) {
        // Removal is only safe once everything the backup holds has arrived.
        // An extra can be the stick's own copy of a file whose backup copy
        // just failed to write -- under a spelling of its name nothing here
        // recognises -- and removing it would leave the stick with neither.
        summary.warnings.push_back(std::to_string(extras.size())
                                   + " file(s) or folder(s) not in the backup were left in place, because not "
                                     "everything in the backup could be restored");
    } else if (options.exact && !extras.empty()) {
        report(RestoreProgress::Phase::Removing);
        // Files first, then directories deepest-first so they are empty.
        std::sort(extras.begin(), extras.end(), [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
        for (const std::string &extra : extras) {
            fs::path target = options.targetRoot / pathFromUtf8(extra);
            const bool isDirectory = fs::is_directory(longPathSafe(target), ec);
            ec.clear();
            if (!isDirectory && extraIsBackupFile(options.targetRoot, target, plan)) {
                continue;  // the restored file itself, under a spelling of its name
            }
            // A directory goes only if it is empty by now, which the
            // deepest-first order above has seen to.
            std::string failure;
            if (infrastructure::removeEntry(target, failure)) {
                ++summary.extrasRemoved;
            } else {
                summary.warnings.push_back(extra + ": could not remove: " + failure);
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

    // A restore that put back files the backup only holds part of has
    // not restored the library, whatever the counts say. It is the one
    // thing the page must not report as a clean Restored.
    bool problems = !summary.rejected.empty() || !summary.writeErrors.empty() || !summary.partial.empty()
                    || (summary.missingTrackPaths && !summary.missingTrackPaths->empty()) || !summary.warnings.empty();
    summary.status = problems ? RestoreSummary::Status::RestoredWithProblems : RestoreSummary::Status::Restored;
    return summary;
}

}  // namespace seabass::application
