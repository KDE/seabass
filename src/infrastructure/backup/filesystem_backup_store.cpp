#include "infrastructure/backup/filesystem_backup_store.hpp"

#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/work_counters.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>

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
            total += entry.file_size(ec);
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

std::string originValue(BackupOrigin origin)
{
    return origin == BackupOrigin::UserRequested ? "user" : "automatic";
}

}  // namespace

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

    auto [written, archiveBytes] = writeArchiveEntries(dir, filePaths);

    {
        std::ofstream manifest(dir / ManifestFileName, std::ios::app);
        manifest << ManifestVersionKey << '\t' << ManifestFormatVersion << '\n';
        manifest << OriginKey << '\t' << originValue(origin) << '\n';
        for (const auto &[entryName, recorded] : written) {
            manifest << entryName << '\t' << recorded << '\n';
        }
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
        if (auto reader = stick_backup::Zip64Reader::tryOpen(existing)) {
            carried = reader->entries();
        }
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
                mtime = std::chrono::duration_cast<std::chrono::seconds>(stamp.time_since_epoch()).count();
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
    if (readManifest(dir).version != ManifestFormatVersion) {
        throw std::runtime_error("backup " + id + " is not a record this build wrote");
    }

    auto [written, archiveBytes] = writeArchiveEntries(dir, filePaths);
    {
        std::ofstream manifest(dir / ManifestFileName, std::ios::app);
        for (const auto &[entryName, recorded] : written) {
            manifest << entryName << '\t' << recorded << '\n';
        }
    }
    stateFor(dir).sizeBytes = archiveBytes;

    BackupRecord record;
    record.id = id;
    record.path = dir.string();
    record.sizeBytes = archiveBytes;
    for (const auto &[entryName, recorded] : readManifest(dir).entries) {
        record.filePaths.push_back(recorded);
    }
    return record;
}

bool FilesystemBackupStore::restoreFromArchive(const fs::path &dir,
                                               const std::vector<std::pair<std::string, std::string>> &entries)
{
    std::error_code ec;
    if (!fs::exists(dir / ArchiveFileName, ec)) {
        return false;
    }
    stick_backup::PosixArchiveFile file(dir / ArchiveFileName,
                                        stick_backup::PosixArchiveFile::OpenMode::ReadOnly);
    std::string error;
    auto reader = stick_backup::Zip64Reader::tryOpen(file, &error);
    if (!reader.has_value()) {
        return false;  // damaged archive: say so rather than restore a prefix
    }

    bool anyRestored = false;
    for (const auto &[entryName, originalPath] : entries) {
        auto index = reader->findEntry(entryName);
        if (!index.has_value()) {
            continue;
        }
        // Refuse a mismatch rather than write bytes that failed their own
        // checksum over a live file -- exactly the case restore exists for.
        if (!reader->verifyCrc(*index)) {
            continue;
        }
        const std::string contents = reader->readEntryToString(*index);
        const fs::path target = resolveRecordedPath(originalPath);
        fs::create_directories(target.parent_path(), ec);
        const bool restored = writeFileDurablyAtomic(target.string(), contents);
        if (restored && target.extension() == ".db") {
            // A database put back next to a -wal or -journal left by a
            // crash mid-save would have those frames replayed over it on
            // the next open: the restore silently undone, or worse, a
            // mix of two generations. The archive holds the state to go
            // back to; the sidecars belong to the state being abandoned.
            for (const char *sidecar : {"-wal", "-shm", "-journal"}) {
                fs::path side = target;
                side += sidecar;
                bool inArchive = false;
                for (const auto &[otherEntry, otherPath] : entries) {
                    if (resolveRecordedPath(otherPath) == side) {
                        inArchive = true;
                        break;
                    }
                }
                if (!inArchive) {
                    std::error_code sideEc;
                    fs::remove(side, sideEc);
                }
            }
        }
        anyRestored = restored || anyRestored;
    }
    return anyRestored;
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
            state.sizeBytes += entry.file_size(ec);
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

std::uint64_t FilesystemBackupStore::prune(size_t keepCount)
{
    std::vector<BackupRecord> automatic;
    for (auto &record : list()) {  // oldest first
        if (record.origin == BackupOrigin::Automatic) {
            automatic.push_back(std::move(record));
        }
    }
    if (automatic.size() <= keepCount) {
        return 0;
    }

    std::uint64_t freed = 0;
    size_t toRemove = automatic.size() - keepCount;
    for (size_t i = 0; i < toRemove; ++i) {
        std::error_code ec;
        fs::remove_all(automatic[i].path, ec);
        if (!ec) {
            freed += automatic[i].sizeBytes;
        }
    }
    return freed;
}

std::uint64_t FilesystemBackupStore::releaseAutomaticBackups(std::uint64_t bytesWanted)
{
    if (bytesWanted == 0) {
        return 0;
    }
    std::vector<BackupRecord> automatic;
    for (auto &record : list()) {  // oldest first
        if (record.origin == BackupOrigin::Automatic) {
            automatic.push_back(std::move(record));
        }
    }
    // The newest automatic record is the one Undo Last Save needs, so it
    // is never released here. A stick tight enough that even that has to
    // go is not a situation to resolve by quietly deleting the only undo
    // the user has left.
    if (automatic.size() <= 1) {
        return 0;
    }

    std::uint64_t freed = 0;
    for (size_t i = 0; i + 1 < automatic.size() && freed < bytesWanted; ++i) {
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

bool FilesystemBackupStore::restore(const std::string &id)
{
    fs::path dir = fs::path(m_baseDirectory) / id;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return false;
    }
    auto manifest = readManifest(dir);
    if (manifest.entries.empty()) {
        return false;  // nothing was ever backed up for this id
    }
    if (manifest.version != ManifestFormatVersion) {
        // Not a shape this build wrote: refuse rather than misinterpret
        // it. This is the only reason the version line still exists.
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
        if (fs::exists(target, ec)) {
            currentPaths.push_back(target.string());
        }
    }
    if (!currentPaths.empty()) {
        // The user asked for this restore, so the copy of what it is
        // about to overwrite is theirs and Seabass never releases it.
        backup(currentPaths, "pre-restore", BackupOrigin::UserRequested);
    }

    return restoreFromArchive(dir, manifest.entries);
}

bool FilesystemBackupStore::remove(const std::string &id)
{
    fs::path dir = fs::path(m_baseDirectory) / id;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return false;
    }
    return fs::remove_all(dir, ec) > 0;
}

}  // namespace seabass::infrastructure::backup
