// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/backup/filesystem_backup_store.hpp"

#include "infrastructure/backup/stick_space.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/fs_remove.hpp"
#include "infrastructure/file_clock.hpp"
#include "infrastructure/work_counters.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <fstream>
#include <set>
#include <exception>
#include <optional>
#include <vector>
#include <span>
#include <sstream>
#include <tuple>

#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"
#include "infrastructure/stick_backup/zip64_writer.hpp"

namespace seabass::infrastructure::backup
{

namespace fs = std::filesystem;
using application::BackupOrigin;
using application::BackupRecord;

namespace
{

// Metadata filenames inside each backup directory. Prefixed with "." so a
// user browsing the directory by hand (the fallback path this store was
// designed for -- see the class comment) sees them as incidental, and so
// they never collide with a real backed-up file's basename.
constexpr const char *ManifestFileName = ".manifest";
constexpr const char *DescriptionFileName = ".description";
constexpr const char *OriginKey = "ORIGIN";

std::string timestampNow()
{
    return std::format("{:%Y%m%dT%H%M%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

std::string sanitize(const std::string &label)
{
    std::string result = label;
    std::replace_if(
        result.begin(), result.end(), [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '-');
    return result;
}

std::uint64_t directorySize(const fs::path &dir)
{
    std::uint64_t total = 0;
    std::error_code ec;
    for (const auto &entry : fs::recursive_directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().filename() != ManifestFileName &&
            entry.path().filename() != DescriptionFileName) {
            // A failed file_size() returns uintmax_t(-1); adding it
            // makes the record about 16 exabytes, and the caller that
            // frees space then stops after one backup believing it has
            // reclaimed enough.
            std::error_code sizeEc;
            const std::uintmax_t bytes = entry.file_size(sizeEc);
            if (!sizeEc) {
                total += bytes;
            }
        }
    }
    return total;
}

std::string readWholeFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The manifest: a "MANIFEST-VERSION" line, an "ORIGIN" line, then one
// "<entry name inside backup.zip>\t<original path>" line per file.
//
// A path under the stick this store lives on is recorded RELATIVE to the
// stick root, because a stick does not come back at the same mount point
// after a reboot and gets whatever drive letter is free on Windows;
// anything genuinely off the stick is recorded absolute.
//
// One format, one layout. Seabass is pre-1.0 and the sticks get written
// anew, so nothing here reads a shape an earlier build wrote -- the
// loose-file layout and its two older manifest versions are gone rather
// than carried. The version line stays for one reason only: restore()
// refuses a manifest it does not recognise instead of misinterpreting
// one, and that guard is worth a line.
constexpr int ManifestFormatVersion = 4;
constexpr const char *ManifestVersionKey = "MANIFEST-VERSION";
constexpr const char *ArchiveFileName = "backup.zip";

struct Manifest
{
    int version = 0;
    std::vector<std::pair<std::string, std::string>> entries;
    std::optional<BackupOrigin> origin;
};

// A manifest with no version line is not one of ours: version stays 0 and
// restore() refuses it.
Manifest readManifest(const fs::path &dir)
{
    Manifest manifest;
    std::ifstream in(dir / ManifestFileName);
    if (!in.is_open()) {
        return manifest;
    }
    std::string line;
    while (std::getline(in, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        std::string key = line.substr(0, tab);
        std::string value = line.substr(tab + 1);
        if (key == ManifestVersionKey) {
            manifest.version = std::atoi(value.c_str());
        } else if (key == OriginKey) {
            manifest.origin = value == "user" ? BackupOrigin::UserRequested : BackupOrigin::Automatic;
        } else {
            manifest.entries.emplace_back(std::move(key), std::move(value));
        }
    }
    return manifest;
}

std::string megabytes(std::uint64_t bytes)
{
    return std::format("{:.1f}", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

std::string originValue(BackupOrigin origin)
{
    return origin == BackupOrigin::UserRequested ? "user" : "automatic";
}

// The manifest, written whole and fsynced: it is the record's commit
// marker (list() wants it), so a stick pulled right after a save finds
// all of it or none -- a partial one would have restore() put back
// fewer files than were backed up. One writer for backup() and
// addToArchive(), so the format has one source beside readManifest().
bool writeManifest(const fs::path &dir, const Manifest &manifest)
{
    std::ostringstream out;
    out << ManifestVersionKey << '\t' << manifest.version << '\n';
    if (manifest.origin) {
        out << OriginKey << '\t' << originValue(*manifest.origin) << '\n';
    }
    for (const auto &[entryName, recorded] : manifest.entries) {
        out << entryName << '\t' << recorded << '\n';
    }
    return writeFileDurablyAtomic((dir / ManifestFileName).string(), out.str());
}

// Puts an archive back to the length it had before an append that
// failed part-way. Through the archive file itself, truncate then
// barrier, as ArchiveUpdater::abort() does: a bare resize_file() stays in
// the page cache, and a stick pulled after the refused save would come
// back with the trailing bytes on the medium and the record refused
// whole. If even that fails the record is removed: an archive with bytes
// after its end-of-central-directory lists as restorable and then is
// not, which is worse than no record.
void cutArchiveBackTo(const fs::path &dir, std::uint64_t length)
{
    const fs::path archivePath = dir / ArchiveFileName;
    // Only an archive that grew needs cutting. One that could not even be
    // opened for writing (read-only, held by another program) never took
    // a byte and is intact; one whose size cannot be read is unknown, and
    // a record removed on a guess is worse than one left alone.
    std::error_code sizeEc;
    const std::uintmax_t size = fs::file_size(archivePath, sizeEc);
    if (sizeEc || size == length) {
        return;
    }
    try {
        if (length == 0) {
            fs::remove(archivePath);
        } else {
            stick_backup::PosixArchiveFile file(archivePath, stick_backup::PosixArchiveFile::OpenMode::ReadWrite);
            file.truncate(length);
            file.barrier();
        }
    } catch (...) {
        std::error_code removeEc;
        fs::remove_all(dir, removeEc);
    }
}

// A directory holding the store's archive but no manifest is a backup
// that died before its manifest: a pull or a crash between the two, or,
// before archives were cut back on failure, a stick that filled up.
// Earlier builds left those on sticks, and hidden from list() they would
// be space nobody could reclaim -- prune and Manage Backups only see
// what is listed. Swept from the space-reclaiming paths only, which run
// under the stick's write lock, so no backup can be writing one; and
// judged by the record's own timestamp id, which a wrong clock or a
// suspend cannot shift the way a file time can: a day old is dead. A
// user's own folder carries no archive and is left alone.
void sweepDeadRecords(const fs::path &base)
{
    const std::string cutoff = std::format(
        "{:%Y%m%dT%H%M%S}",
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now() - std::chrono::hours(24)));
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(base, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        // "YYYYMMDDTHHMMSS-label", as timestampNow() + sanitize() make it.
        bool timestamped = name.size() > 15 && name[8] == 'T' && name[15] == '-';
        for (size_t i = 0; timestamped && i < 15; ++i) {
            timestamped = i == 8 || std::isdigit(static_cast<unsigned char>(name[i]));
        }
        if (!timestamped || name.compare(0, 15, cutoff) >= 0) {
            continue;
        }
        // A stat that FAILED is not an answer. is_regular_file reports
        // "not there" and "I could not look" the same way -- plain
        // false -- so a transient failure on the manifest of a
        // complete, valid record reads as "an archive with no manifest"
        // and this deletes somebody's backup. The project's own rule
        // for exactly this is in fs_remove.cpp's stillThere(): a stat
        // that fails for any reason other than "not found" counts as
        // still there.
        //
        // Reached from releaseAutomaticBackups() on every save to a
        // tight stick, so it runs often and on the sticks that are
        // least healthy.
        // "Not there" is an answer; any other error is not.
        //
        // is_regular_file() sets ec to ENOENT for a file that simply
        // does not exist, so refusing on any ec at all would stop this
        // sweeping the dead records it exists for -- which is what the
        // first version of this fix did, and what the case below caught.
        // The distinction is the one fs_remove.cpp's stillThere() draws.
        auto looked = [](const fs::path &path, bool &answered) {
            std::error_code ec;
            const bool yes = fs::is_regular_file(path, ec);
            answered = !ec || ec == std::errc::no_such_file_or_directory;
            return yes;
        };
        bool manifestAnswered = false;
        bool archiveAnswered = false;
        const bool hasManifest = looked(entry.path() / ManifestFileName, manifestAnswered);
        const bool hasArchive = looked(entry.path() / ArchiveFileName, archiveAnswered);
        if (!manifestAnswered || !archiveAnswered) {
            continue;  // could not look, so this is not a record to judge
        }
        if (hasManifest || !hasArchive) {
            continue;
        }
        fs::remove_all(entry.path(), ec);
    }
}

}  // namespace

struct FilesystemBackupStore::OpenedArchive
{
    std::unique_ptr<stick_backup::PosixArchiveFile> file;
    std::optional<stick_backup::Zip64Reader> reader;  // points into *file
    std::vector<std::size_t> indexes;                 // one per manifest entry, in order
};

// The stick this store lives on: baseDirectory is <stick>/Seabass/backups.
fs::path FilesystemBackupStore::stickRoot() const
{
    // baseDirectory is <stick>/Seabass/backups, so the stick root is two
    // levels up, not one. It was one level while backups lived in
    // <stick>/.seabass-backups, and getting this wrong is quiet and
    // nasty: every recorded path would be stored relative to
    // <stick>/Seabass, and restore would resolve it to a path inside the
    // Seabass directory instead of back to the real file.
    return fs::path(m_baseDirectory).parent_path().parent_path();
}

// What goes in the manifest for `source`: relative to the stick when it is
// on the stick, absolute otherwise.
std::string FilesystemBackupStore::recordedPathFor(const fs::path &source) const
{
    const fs::path absolute = fs::absolute(source).lexically_normal();
    const fs::path root = stickRoot().lexically_normal();
    const fs::path relative = absolute.lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..") {
        return absolute.string();  // genuinely off the stick
    }
    return relative.generic_string();
}

// The reverse: a relative manifest entry names a file on whichever stick
// this store is on *now*, which is the whole point of recording it that way.
fs::path FilesystemBackupStore::resolveRecordedPath(const std::string &recorded) const
{
    const fs::path path(recorded);
    if (path.is_absolute()) {
        return path;
    }
    // lexically_normal() also converts to the platform's preferred
    // separator -- needed because `recorded` came from a manifest as a
    // forward-slash generic_string(), and operator/() only inserts a
    // native separator at the join point, it doesn't rewrite separators
    // already inside `path`'s own components. Without this, the result
    // is a mix of '\' and '/' on Windows: still a valid path to the OS,
    // but not string-equal to any path built the ordinary component-by-
    // component way, which is what callers compare it against.
    return (stickRoot() / path).lexically_normal();
}

FilesystemBackupStore::FilesystemBackupStore(std::string baseDirectory) : m_baseDirectory(std::move(baseDirectory)) {}

BackupRecord FilesystemBackupStore::backup(const std::vector<std::string> &filePaths, const std::string &label,
                                          BackupOrigin origin)
{
    // timestampNow() has second resolution and a save can make several
    // records inside one second, so a directory that already exists is
    // never reused: two records landing in one directory would overwrite
    // each other's contents, defeating the point of backing up first.
    std::string baseId = timestampNow() + "-" + sanitize(label);
    std::string id = baseId;
    fs::path dir = fs::path(m_baseDirectory) / id;
    for (int suffix = 1; fs::exists(dir); ++suffix) {
        id = baseId + "-" + std::to_string(suffix);
        dir = fs::path(m_baseDirectory) / id;
    }
    fs::create_directories(dir);

    // A backup that fails part-way -- the stick ran out of space while
    // the archive was being written, as the release rig's full-stick
    // check does on purpose -- must not leave its directory behind: a
    // truncated backup.zip with no manifest is listed by list() as a
    // record (it keeps any directory holding an archive), shows up in
    // Manage Backups as an empty entry, and would be offered to restore
    // from. The directory is this call's own, so removing it whole on
    // any failure loses nothing that was ever complete.
    std::vector<std::pair<std::string, std::string>> written;
    std::uint64_t archiveBytes = 0;
    try {
        std::tie(written, archiveBytes) = writeArchiveEntries(dir, filePaths);
        Manifest manifest;
        manifest.version = ManifestFormatVersion;
        manifest.origin = origin;
        manifest.entries = written;
        if (!writeManifest(dir, manifest)) {
            throw std::runtime_error("could not write the manifest of backup " + id + " under " + m_baseDirectory);
        }
    } catch (...) {
        std::error_code removeEc;
        fs::remove_all(dir, removeEc);
        throw;
    }

    DirectoryState &state = stateFor(dir);
    state.sizeBytes = archiveBytes;

    BackupRecord record;
    record.id = id;
    record.path = dir.string();
    record.label = label;
    record.origin = origin;
    record.sizeBytes = archiveBytes;
    for (const auto &[entryName, recorded] : written) {
        record.filePaths.push_back(recorded);
    }
    return record;
}


std::pair<std::vector<std::pair<std::string, std::string>>, std::uint64_t>
FilesystemBackupStore::writeArchiveEntries(const fs::path &dir, const std::vector<std::string> &filePaths)
{
    // Carry whatever the archive already holds, so an append lists the
    // earlier entries in the new central directory too.
    std::vector<stick_backup::CentralEntry> carried;
    const fs::path archivePath = dir / ArchiveFileName;
    std::error_code ec;
    if (fs::exists(archivePath, ec)) {
        stick_backup::PosixArchiveFile existing(archivePath, stick_backup::PosixArchiveFile::OpenMode::ReadOnly);
        std::string error;
        auto reader = stick_backup::Zip64Reader::tryOpen(existing, &error);
        if (!reader) {
            // Appending past an archive the reader refuses would write a
            // central directory naming only the new entries, under a
            // manifest that still names the old ones: a record that lists
            // as restorable and then is not. Refusing makes the save say
            // "could not back up", which is the truth.
            throw std::runtime_error("backup archive " + archivePath.string() + " is unreadable, so nothing is added to it: "
                                     + error);
        }
        carried = reader->entries();
    }

    std::vector<std::pair<std::string, std::string>> written;
    std::uint64_t archiveBytes = 0;
    {
        stick_backup::PosixArchiveFile file(archivePath, stick_backup::PosixArchiveFile::OpenMode::ReadWrite);
        stick_backup::Zip64Writer writer(file, carried);
        for (const auto &filePath : filePaths) {
            fs::path source(filePath);
            if (!fs::exists(source, ec)) {
                continue;
            }
            // The recorded path doubles as the entry name, so files that
            // share a basename need no _1/_2 disambiguation at all -- the
            // clash the loose layout has to guard against cannot arise.
            const std::string recorded = recordedPathFor(source);
            std::string entryName = recorded;
            std::replace(entryName.begin(), entryName.end(), '\\', '/');
            while (!entryName.empty() && entryName.front() == '/') {
                entryName.erase(entryName.begin());
            }
            if (entryName.empty()) {
                continue;
            }
            const std::string contents = readWholeFile(source);
            std::int64_t mtime = 0;
            if (auto stamp = fs::last_write_time(source, ec); !ec) {
                // fs::file_time_type's epoch is unspecified pre-C++20 and,
                // even now, implementation-defined in practice: libstdc++
                // happens to share system_clock's Unix epoch, but MSVC's
                // STL uses the Windows FILETIME epoch (1601) internally,
                // so a raw time_since_epoch() here is only ever correct
                // by accident on the platforms this project first shipped
                // on. Confirmed directly on MSVC: it produced 2147483647
                // (an int32 clamp of a wildly wrong ~13-trillion-second
                // value), silently poisoning every mtime this store wrote
                // and tripping Zip64Reader's DOS-vs-extended-timestamp
                // consistency check on every restore. toSystemClock()
                // (file_clock.hpp) is the same conversion this project
                // already does everywhere else it needs a real Unix
                // timestamp from fs::last_write_time() (see cli/main.cpp,
                // stick_tree_walker.cpp's toUnixSeconds(), etc.) --
                // inlined rather than reusing that helper, so this file
                // does not gain a dependency on stick_tree_walker.cpp in
                // every target that lists it directly.
                const auto sysTime = infrastructure::toSystemClock(stamp);
                mtime = std::chrono::duration_cast<std::chrono::seconds>(sysTime.time_since_epoch()).count();
            }
            writer.addFileFromMemory(entryName, mtime,
                                     std::as_bytes(std::span<const char>(contents.data(), contents.size())),
                                     nullptr, stick_backup::Compression::Deflate);
            written.emplace_back(entryName, recorded);
        }
        // A central directory after every call, so the record is complete
        // and readable at every point a crash could happen -- the loose
        // layout's guarantee, kept.
        writer.finish("{}", "backup-manifest.json", 0);
        file.barrier();
        // Counted like any other durable whole-file write: this is the
        // barrier that costs ~118 ms on a stick, and the whole point of
        // the archive is that a save pays it once instead of per file.
        // Leaving it uncounted would have made the backup half of a save
        // invisible to the very counter that measures it.
        WorkCounters::instance().noteDurableFileWrite();
        archiveBytes = file.size();
    }
    return {std::move(written), archiveBytes};
}

BackupRecord FilesystemBackupStore::addToArchive(const std::string &id, const std::vector<std::string> &filePaths)
{
    fs::path dir = fs::path(m_baseDirectory) / id;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        throw std::runtime_error("no backup with id " + id + " to add to");
    }
    Manifest manifest = readManifest(dir);
    if (manifest.version != ManifestFormatVersion) {
        throw std::runtime_error("backup " + id + " is not a record this build wrote");
    }

    // An append that fails part-way (the stick fills up on the second
    // file of a label) leaves the new entry's bytes after the old
    // end-of-central-directory, and any trailing byte makes the reader
    // refuse the whole archive -- the files backed up before it,
    // correctly, included, so the undo would fail for them too. Cut the
    // archive back to where it was and leave the manifest alone: the
    // record stays what it was before the call.
    const fs::path archivePath = dir / ArchiveFileName;
    std::error_code sizeEc;
    std::uintmax_t archiveBefore = fs::file_size(archivePath, sizeEc);
    if (sizeEc == std::errc::no_such_file_or_directory) {
        archiveBefore = 0;
    } else if (sizeEc) {
        // Refused before anything is written: a length taken from a
        // failed stat would have the cut-back remove a good archive.
        throw std::runtime_error("could not read the size of " + archivePath.string() + ": " + sizeEc.message());
    }
    std::vector<std::pair<std::string, std::string>> written;
    std::uint64_t archiveBytes = 0;
    try {
        std::tie(written, archiveBytes) = writeArchiveEntries(dir, filePaths);
        // The manifest this call parsed, with the new entries: rewritten
        // whole from what was read, never spliced onto raw bytes.
        manifest.entries.insert(manifest.entries.end(), written.begin(), written.end());
        if (!writeManifest(dir, manifest)) {
            throw std::runtime_error("could not write the manifest of backup " + id + " under " + m_baseDirectory);
        }
    } catch (...) {
        cutArchiveBackTo(dir, archiveBefore);
        throw;
    }
    stateFor(dir).sizeBytes = archiveBytes;

    BackupRecord record;
    record.id = id;
    record.path = dir.string();
    record.sizeBytes = archiveBytes;
    for (const auto &[entryName, recorded] : manifest.entries) {
        record.filePaths.push_back(recorded);
    }
    return record;
}

bool FilesystemBackupStore::restoreFromArchive(const OpenedArchive &opened,
                                               const std::vector<std::pair<std::string, std::string>> &entries,
                                               std::string *failure, std::size_t *filesWritten)
{
    *filesWritten = 0;
    std::error_code ec;
    std::set<fs::path> inArchive;
    for (const auto &[entryName, originalPath] : entries) {
        inArchive.insert(resolveRecordedPath(originalPath));
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto &[entryName, originalPath] = entries[i];
        const std::size_t index = opened.indexes[i];
        // Stopped at the first failure, so the reason names it: going on
        // would let a later, different failure overwrite the reason (a
        // stick out of room reported as a damaged backup) and put back
        // more files the caller then has to roll back.
        std::string contents;
        try {
            contents = opened.reader->readEntryToString(index);
        } catch (const std::exception &e) {
            *failure = entryName + " in the backup's archive cannot be read: " + e.what();
            return false;
        }
        const fs::path target = resolveRecordedPath(originalPath);
        fs::create_directories(target.parent_path(), ec);
        if (!writeFileDurablyAtomic(target.string(), contents)) {
            // A write, not a read: the archive was fine, the volume was not.
            *failure = "could not write " + target.string() + " (" + megabytes(availableBytes(target.parent_path()))
                       + " MB free there)";
            return false;
        }
        ++*filesWritten;
        constexpr std::int64_t Year2000 = 946'684'800;
        const std::int64_t recorded = opened.reader->entries()[index].mtimeUnix;
        if (recorded > Year2000) {
            std::error_code timeEc;
            // Header-only toFileClock(), as the write side does, for the
            // same reason: the file clock's epoch is not the Unix one
            // (MSVC counts from 1601), and this file does not depend on
            // stick_tree_walker.cpp. Apple's libc++ has no clock_cast at
            // all, so a bare one does not compile there.
            const std::chrono::system_clock::time_point asSystem{std::chrono::seconds(recorded)};
            fs::last_write_time(target, infrastructure::toFileClock(asSystem), timeEc);
        }
        if (target.extension() == ".db") {
            // A database put back next to a -wal or -journal left by a
            // crash mid-save would have those frames replayed over it on
            // the next open: the restore silently undone, or worse, a
            // mix of two generations. The archive holds the state to go
            // back to; the sidecars belong to the state being abandoned.
            for (const char *sidecar : {"-wal", "-shm", "-journal"}) {
                fs::path side = target;
                side += sidecar;
                if (inArchive.contains(side)) {
                    continue;
                }
                // Through removeEntry(), and the result is the restore's
                // result. The sweep in #35 put every -wal/-shm site under
                // "a generated name, and a failure is cleanup noise" --
                // true of the others, not of this one. The paragraph
                // above says what a surviving sidecar does: its frames
                // are replayed over the file on the next open, and the
                // restore is undone or half-undone. Reporting that as a
                // completed restore is the worst answer available.
                //
                // The realistic trigger here is Windows rather than
                // decomposition: something still has the -wal open, and
                // the delete is refused.
                std::string sidecarFailure;
                if (!infrastructure::removeEntry(side, sidecarFailure)) {
                    *failure = "restored " + target.string() + ", but " + side.filename().string()
                               + " beside it could not be removed, and its contents would be replayed over the "
                                 "restored file: " + sidecarFailure;
                    return false;
                }
            }
        }
    }
    return true;
}

FilesystemBackupStore::DirectoryState &FilesystemBackupStore::stateFor(const fs::path &dir)
{
    auto it = m_directoryState.find(dir.string());
    if (it != m_directoryState.end()) {
        return it->second;
    }
    DirectoryState state;
    std::error_code ec;
    for (const auto &entry : fs::recursive_directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name != ManifestFileName && name != DescriptionFileName) {
            std::error_code sizeEc;  // see directorySize(): -1 would make this 16 EB
            const std::uintmax_t bytes = entry.file_size(sizeEc);
            if (!sizeEc) {
                state.sizeBytes += bytes;
            }
        }
    }
    return m_directoryState.emplace(dir.string(), std::move(state)).first->second;
}

std::vector<BackupRecord> FilesystemBackupStore::list()
{
    std::vector<BackupRecord> records;
    std::error_code ec;
    if (!fs::is_directory(m_baseDirectory, ec)) {
        return records;
    }

    for (const auto &entry : fs::directory_iterator(m_baseDirectory, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        // Only directories this store wrote. Anything else under the
        // backups folder (a folder the user put there, another tool's
        // output) used to be listed as an Automatic record and, sorting
        // first, be the first thing prune() removed.
        // The manifest is written last, whole, and is the record's commit
        // marker. A directory holding only backup.zip is a backup that
        // died between its archive and its manifest (a pull, a crash, and
        // under earlier builds a stick that filled up) and not a record:
        // listed, it showed in Manage Backups as an empty entry and, with
        // no origin line to read, counted as automatic and pushed real
        // records out of prune's keep window. prune() sweeps it once it is
        // a day old (sweepDeadRecords), so it does not sit unlisted on a
        // stick taking space nobody could reclaim.
        std::error_code manifestEc;
        if (!fs::is_regular_file(entry.path() / ManifestFileName, manifestEc)) {
            continue;
        }
        BackupRecord record;
        record.id = entry.path().filename().string();
        record.path = entry.path().string();
        size_t dash = record.id.find('-');
        record.label = dash == std::string::npos ? "" : record.id.substr(dash + 1);
        record.description = readWholeFile(entry.path() / DescriptionFileName);
        record.sizeBytes = directorySize(entry.path());
        const Manifest manifest = readManifest(entry.path());
        record.origin = manifest.origin.value_or(BackupOrigin::Automatic);
        for (const auto &[onDisk, originalPath] : manifest.entries) {
            record.filePaths.push_back(resolveRecordedPath(originalPath).string());
        }
        records.push_back(std::move(record));
    }

    // Backup ids are timestamp-prefixed, so lexical order is chronological.
    std::sort(records.begin(), records.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    return records;
}

std::vector<BackupRecord> FilesystemBackupStore::pruneCandidates(size_t keepCount)
{
    std::vector<BackupRecord> automatic;
    for (auto &record : list()) {  // oldest first
        if (record.origin == BackupOrigin::Automatic) {
            automatic.push_back(std::move(record));
        }
    }
    if (automatic.size() <= keepCount) {
        return {};
    }
    automatic.resize(automatic.size() - keepCount);
    return automatic;
}

application::PruneResult FilesystemBackupStore::prune(size_t keepCount)
{
    sweepDeadRecords(m_baseDirectory);
    const std::vector<BackupRecord> automatic = pruneCandidates(keepCount);
    application::PruneResult result;
    for (const BackupRecord &record : automatic) {
        std::error_code ec;
        fs::remove_all(record.path, ec);
        // remove_all() answers "how many did I remove", and 0 with no
        // error is the directory having gone already -- which is fine,
        // it is not there any more either way. What is not fine is
        // counting bytes freed for a record still sitting on the stick:
        // a refusal here (a read-only folder, a name this filesystem
        // will not resolve for unlink) used to leave the caller
        // reporting nothing freed and nothing wrong.
        if (ec || fs::exists(record.path)) {
            result.failed++;
            continue;
        }
        result.removed++;
        result.bytesFreed += record.sizeBytes;
    }
    return result;
}

std::uint64_t FilesystemBackupStore::releaseAutomaticBackups(std::uint64_t bytesWanted,
                                                            const std::set<std::string> &spare)
{
    sweepDeadRecords(m_baseDirectory);
    if (bytesWanted == 0) {
        return 0;
    }
    std::vector<BackupRecord> automatic;
    for (auto &record : list()) {  // oldest first
        if (record.origin == BackupOrigin::Automatic) {
            automatic.push_back(std::move(record));
        }
    }
    // The newest automatic record is never released here, and neither is
    // anything in `spare` -- the records of the save that just finished,
    // which together are what Undo Last Save restores. A stick tight
    // enough that even those have to go is not a situation to resolve by
    // quietly deleting the only undo the user has left.
    if (automatic.size() <= 1) {
        return 0;
    }

    std::uint64_t freed = 0;
    for (size_t i = 0; i + 1 < automatic.size() && freed < bytesWanted; ++i) {
        if (spare.contains(automatic[i].id)) {
            continue;
        }
        std::error_code ec;
        fs::remove_all(automatic[i].path, ec);
        if (!ec) {
            freed += automatic[i].sizeBytes;
        }
    }
    return freed;
}

void FilesystemBackupStore::setDescription(const std::string &id, const std::string &description)
{
    fs::path dir = fs::path(m_baseDirectory) / id;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return;
    }
    std::ofstream out(dir / DescriptionFileName, std::ios::trunc);
    out << description;
}

bool FilesystemBackupStore::isRestorable(const std::string &id) const
{
    const fs::path dir = fs::path(m_baseDirectory) / id;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return false;
    }
    const Manifest manifest = readManifest(dir);
    return !manifest.entries.empty() && manifest.version == ManifestFormatVersion
           && fs::is_regular_file(dir / ArchiveFileName, ec);
}

bool FilesystemBackupStore::restore(const std::string &id)
{
    m_lastRestoreError.clear();
    m_lastPreRestoreId.reset();
    fs::path dir = fs::path(m_baseDirectory) / id;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        m_lastRestoreError = "backup " + id + " is not on the stick";
        return false;
    }
    auto manifest = readManifest(dir);
    if (manifest.entries.empty()) {
        m_lastRestoreError = "backup " + id + " lists no files";
        return false;  // nothing was ever backed up for this id
    }
    if (manifest.version != ManifestFormatVersion) {
        // Not a shape this build wrote: refuse rather than misinterpret
        // it. This is the only reason the version line still exists.
        m_lastRestoreError = "backup " + id + " is not a record this build wrote";
        return false;
    }

    // The archive first, before anything is copied or written: a backup
    // that is missing, damaged or short of an entry is refused with
    // nothing else done -- the pre-restore copy below used to be made
    // first, and stayed on the stick, a permanent record of files that
    // were never touched.
    OpenedArchive opened;
    if (!openArchive(dir, manifest.entries, &m_lastRestoreError, opened)) {
        return false;
    }

    // Preserve the "always back up before writing" invariant for restore
    // itself: the current on-disk contents of every target path get their
    // own backup (label "pre-restore") before being overwritten. Same for
    // both layouts -- it is keyed on the recorded original path, which v3
    // did not change.
    std::vector<std::string> currentPaths;
    for (const auto &[onDisk, originalPath] : manifest.entries) {
        const fs::path target = resolveRecordedPath(originalPath);
        // A file whose stat FAILS is included, not dropped. This list is
        // what gets copied aside before the restore overwrites anything,
        // so a target that is there but could not be examined must be
        // protected rather than quietly written over -- and it is also
        // what the free-space check is computed from.
        std::error_code existsEc;
        if (fs::exists(target, existsEc) || existsEc) {
            currentPaths.push_back(target.string());
        }
    }

    // Refused up front when the stick cannot hold it: putting the files
    // back writes each one whole beside the old (writeFileDurablyAtomic,
    // a temporary file then a rename) and first keeps a copy of what it
    // overwrites, so the room it needs is known before it starts. On a
    // nearly full stick that copy used to be the thing that made the
    // restore impossible, and the failure then read as a damaged backup
    // (issue #27). Measured on every volume involved -- the backups
    // directory's, and each target's, since a recorded path can be
    // absolute and off the stick -- and the smallest counts.
    // A volume that cannot be measured is left out rather than read as
    // full: before this check existed such a restore simply ran, and a
    // write that then fails still says so.
    const std::uint64_t needed = restoreSpaceNeeded(manifest.entries, opened);
    std::optional<std::uint64_t> available;
    const auto measure = [&](const fs::path &path) {
        std::error_code spaceEc;
        const fs::space_info space = fs::space(path, spaceEc);
        if (!spaceEc) {
            available = std::min<std::uint64_t>(available.value_or(space.available), space.available);
        }
    };
    measure(m_baseDirectory);
    for (const std::string &path : currentPaths) {
        measure(fs::path(path).parent_path());
    }
    if (available && *available < needed) {
        m_lastRestoreError = std::format("not enough space on the stick to put {} file(s) back: needs about {} MB, {} MB free",
                                         manifest.entries.size(), megabytes(needed), megabytes(*available));
        return false;
    }
    // Only now inflate every entry to check it: a record refused for space
    // (a large sync, on a USB 2 stick) is refused without that wait.
    if (!verifyArchive(manifest.entries, &m_lastRestoreError, opened)) {
        return false;
    }
    if (!currentPaths.empty()) {
        // The user asked for this restore, so the copy of what it is
        // about to overwrite is theirs and Seabass never releases it.
        try {
            m_lastPreRestoreId = backup(currentPaths, "pre-restore", BackupOrigin::UserRequested).id;
        } catch (const std::exception &e) {
            m_lastRestoreError = std::string("could not keep a copy of what the restore would overwrite: ") + e.what();
            return false;
        }
    }

    std::size_t filesWritten = 0;
    const bool restored = restoreFromArchive(opened, manifest.entries, &m_lastRestoreError, &filesWritten);
    if (!restored && filesWritten == 0 && m_lastPreRestoreId) {
        // Nothing was overwritten, so the copy protects nothing -- and on
        // a stick that has just run out of room it would be the thing
        // taking the room. Once a file has been put back it stays: it is
        // the last safe state if the caller's rollback fails too.
        remove(*m_lastPreRestoreId);
        m_lastPreRestoreId.reset();
    }
    return restored;
}

bool FilesystemBackupStore::openArchive(const fs::path &dir, const std::vector<std::pair<std::string, std::string>> &entries,
                                        std::string *failure, OpenedArchive &opened) const
{
    std::error_code ec;
    if (!fs::exists(dir / ArchiveFileName, ec)) {
        *failure = "the backup's archive is missing";
        return false;
    }
    try {
        opened.file = std::make_unique<stick_backup::PosixArchiveFile>(dir / ArchiveFileName,
                                                                        stick_backup::PosixArchiveFile::OpenMode::ReadOnly);
        std::string openError;
        opened.reader = stick_backup::Zip64Reader::tryOpen(*opened.file, &openError);
        if (!opened.reader.has_value()) {
            *failure = "the backup's archive is damaged: " + openError;
            return false;
        }
        for (const auto &[entryName, originalPath] : entries) {
            auto index = opened.reader->findEntry(entryName);
            if (!index.has_value()) {
                *failure = "the backup's archive lacks " + entryName;
                return false;
            }
            opened.indexes.push_back(*index);
        }
    } catch (const std::exception &e) {
        *failure = std::string("the backup's archive cannot be read: ") + e.what();
        return false;
    }
    return true;
}

bool FilesystemBackupStore::verifyArchive(const std::vector<std::pair<std::string, std::string>> &entries,
                                          std::string *failure, const OpenedArchive &opened) const
{
    try {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (!opened.reader->verifyCrc(opened.indexes[i])) {
                *failure = entries[i].first + " in the backup's archive is damaged (checksum)";
                return false;
            }
        }
    } catch (const std::exception &e) {
        *failure = std::string("the backup's archive cannot be read: ") + e.what();
        return false;
    }
    return true;
}

std::uint64_t FilesystemBackupStore::restoreSpaceNeeded(const std::vector<std::pair<std::string, std::string>> &entries,
                                                        const OpenedArchive &opened) const
{
    // A plain sum of what comes back plus what is there now overstated the
    // peak by about half -- the pre-restore copy is deflated, and each file
    // is renamed into place before the next temporary is written -- and
    // refused undos that would have fitted. The bound here is what the
    // peak cannot exceed: the copy at the files' own size, one temporary
    // file the size of the largest entry, the growth of the files that
    // come back bigger, and a margin for the record's own files and the
    // clusters each written file rounds up to -- per file, since a sync
    // record holds hundreds and a cue record two.
    constexpr std::uint64_t HeadroomBase = 256 * 1024;
    constexpr std::uint64_t HeadroomPerFile = 64 * 1024;
    std::error_code ec;
    std::uint64_t current = 0;
    std::uint64_t largest = 0;
    std::uint64_t growth = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::uint64_t comesBack = opened.reader->entries()[opened.indexes[i]].size;
        const fs::path target = resolveRecordedPath(entries[i].second);
        // One question, one answer. This used to be is_regular_file()
        // and then file_size(), two stats sharing one error_code, and
        // when the file went between them the second came back as
        // uintmax_t(-1) with nothing reading `ec`: `current` wrapped
        // round to one byte less and `growth` got nothing for a file
        // that comes back whole. The estimate went DOWN by the size of
        // that file, and this is the figure that decides whether a
        // restore is refused for space. file_size() alone fails for
        // anything that is not a regular file, which is all the first
        // stat was asking; a size it cannot give is read as nothing
        // there, the same as before for a file that is absent.
        const std::uintmax_t measured = fs::file_size(target, ec);
        const std::uint64_t thereNow = ec ? 0 : measured;
        current += thereNow;
        largest = std::max(largest, comesBack);
        growth += comesBack > thereNow ? comesBack - thereNow : 0;
    }
    return current + largest + growth + HeadroomBase + HeadroomPerFile * entries.size();
}

std::optional<std::uint64_t> FilesystemBackupStore::restoreSpaceNeeded(const std::string &id) const
{
    const fs::path dir = fs::path(m_baseDirectory) / id;
    const Manifest manifest = readManifest(dir);
    OpenedArchive opened;
    std::string ignored;
    if (!openArchive(dir, manifest.entries, &ignored, opened)) {
        return std::nullopt;  // a figure without the archive's sizes would read as a real one
    }
    return restoreSpaceNeeded(manifest.entries, opened);
}

bool FilesystemBackupStore::remove(const std::string &id)
{
    fs::path dir = fs::path(m_baseDirectory) / id;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return false;
    }
    // remove_all() answers static_cast<uintmax_t>(-1) on failure, which
    // is emphatically "> 0", so this used to report a removal that did
    // not happen. discardBackupsTakenThisSave() counts what it removed
    // against what stayed, and both sides of that count came from here.
    const std::uintmax_t removed = fs::remove_all(dir, ec);
    if (ec || removed == static_cast<std::uintmax_t>(-1)) {
        return false;
    }
    return removed > 0;
}

}  // namespace seabass::infrastructure::backup
