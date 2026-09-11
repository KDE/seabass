// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/rekordbox/anlz_source_for_root.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>

#include "infrastructure/local/browsed_backup_root.hpp"
#include "infrastructure/stick_backup/archive_anlz_source.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"

namespace seabass::infrastructure::rekordbox
{

namespace fs = std::filesystem;

namespace
{

// Keyed by the archive's path, with its size and mtime as the freshness
// check: a backup rewritten since (a newer generation, a different
// stick) gets a fresh open rather than the stale index.
struct OpenedArchive
{
    std::uintmax_t size = 0;
    fs::file_time_type mtime;
    std::shared_ptr<AnlzByteSource> source;
};

std::mutex g_openedMutex;
std::map<std::string, OpenedArchive> g_opened;

std::shared_ptr<AnlzByteSource> openArchiveSource(const std::string &archivePath)
{
    std::error_code ec;
    const auto size = fs::file_size(fs::path(archivePath), ec);
    if (ec) {
        return nullptr;
    }
    const auto mtime = fs::last_write_time(fs::path(archivePath), ec);
    if (ec) {
        return nullptr;
    }

    // Look up under the lock, open outside it, insert under the lock. The
    // open reads the whole central directory -- tens of thousands of
    // entries off a slow drive -- and a UI-thread lookup for a backup
    // that is already cached must not wait behind a background scan's
    // first open of a different one. Two threads racing to open the same
    // archive both succeed and the second keeps the first's; a wasted
    // open is cheaper than a lock held across I/O.
    {
        std::lock_guard<std::mutex> lock(g_openedMutex);
        auto it = g_opened.find(archivePath);
        if (it != g_opened.end() && it->second.size == size && it->second.mtime == mtime) {
            return it->second.source;
        }
    }

    std::shared_ptr<AnlzByteSource> source;
    try {
        // The ArchiveFile is kept alive by the source itself, through the
        // Zip64Reader's shared_ptr, for as long as any reader holds it.
        auto file = std::make_shared<stick_backup::PosixArchiveFile>(
            fs::path(archivePath), stick_backup::PosixArchiveFile::OpenMode::ReadOnly);
        auto reader = std::make_shared<const stick_backup::Zip64Reader>(stick_backup::Zip64Reader::open(*file));
        source = std::make_shared<stick_backup::ArchiveAnlzSource>(std::move(reader), "PIONEER/", std::move(file));
    } catch (const std::exception &) {
        // A truncated or replaced archive: the caller browses the
        // extracted catalogs without cues rather than failing the scan.
        std::lock_guard<std::mutex> lock(g_openedMutex);
        g_opened.erase(archivePath);
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(g_openedMutex);
    auto it = g_opened.find(archivePath);
    if (it != g_opened.end() && it->second.size == size && it->second.mtime == mtime) {
        return it->second.source;  // someone else got there first; use theirs
    }
    g_opened[archivePath] = OpenedArchive{size, mtime, source};
    return source;
}

}  // namespace

std::shared_ptr<AnlzByteSource> anlzSourceForPioneerRoot(const std::string &pioneerRoot)
{
    const auto archive = local::browsedBackupArchive(fs::path(pioneerRoot).parent_path());
    if (!archive) {
        return std::make_shared<FilesystemAnlzSource>(pioneerRoot);
    }
    if (auto source = openArchiveSource(archive->string())) {
        return source;
    }
    return std::make_shared<FilesystemAnlzSource>(pioneerRoot);
}

void forgetArchiveSource(const std::string &archivePath)
{
    std::lock_guard<std::mutex> lock(g_openedMutex);
    g_opened.erase(archivePath);
}

}  // namespace seabass::infrastructure::rekordbox
