// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/stick_backup/backup_artwork_lookup.hpp"

#include "infrastructure/engine/engine_artwork.hpp"
#include "infrastructure/engine/engine_sqlite.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"

#include <sqlite3.h>

#include <cstdint>
#include <exception>
#include <fstream>
#include <vector>

namespace seabass::infrastructure::stick_backup
{

namespace fs = std::filesystem;

BackupArtworkLookup::BackupArtworkLookup(std::vector<fs::path> archives) : m_archives(std::move(archives)) {}

BackupArtworkLookup::~BackupArtworkLookup()
{
    if (!m_scratch.empty()) {
        std::error_code ec;
        fs::remove_all(m_scratch, ec);
    }
}

// The track's cover as the backed-up library recorded it: the image its
// AlbumArt row keeps in the database, or else the row's hash spelled the
// way an artwork file is named. Both empty when that library does not know
// the track, or knows it without a cover.
BackupArtworkLookup::TrackArtwork BackupArtworkLookup::artworkForTrack(const fs::path &archive,
                                                                       const std::string &trackPath)
{
    ExtractedDatabase &extracted = m_databases[seabass::pathToUtf8(archive)];
    if (!extracted.tried) {
        extracted.tried = true;
        try {
            PosixArchiveFile file(archive, PosixArchiveFile::OpenMode::ReadOnly);
            const Zip64Reader reader = Zip64Reader::open(file);
            const auto entry = reader.findEntry("Engine Library/Database2/m.db");
            if (entry) {
                if (m_scratch.empty()) {
                    std::error_code ec;
                    m_scratch = fs::temp_directory_path(ec)
                        / ("seabass-backup-art-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
                    fs::create_directories(m_scratch, ec);
                }
                const fs::path destination = m_scratch / (std::to_string(m_databases.size()) + ".db");
                std::ofstream out(destination, std::ios::binary);
                const std::string bytes = reader.readEntryToString(*entry);
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                out.close();
                if (out) {
                    extracted.file = destination;
                }
            }
        } catch (const std::exception &) {
            // Unreadable archive: one fewer place to look.
        }
    }
    if (extracted.file.empty()) {
        return {};
    }
    TrackArtwork found;

    sqlite3 *handle = nullptr;
    if (sqlite3_open_v2(seabass::pathToUtf8(extracted.file).c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (handle != nullptr) {
            sqlite3_close(handle);
        }
        return {};
    }
    sqlite3_stmt *stmt = nullptr;
    // Engine stores the path relative to its own library directory, one
    // level inside the stick root the archive paths start at.
    const std::string enginePath = "../" + trackPath;
    // A schema without the image column keeps no covers in the database.
    bool hasImageColumn = false;
    try {
        hasImageColumn = engine::hasColumn(handle, "AlbumArt", "albumArt");
    } catch (const std::exception &) {
        // Unreadable: asked below, where the query fails the same way.
    }
    const std::string sql = std::string("SELECT a.hash, ") + (hasImageColumn ? "a.albumArt" : "NULL")
        + " FROM Track t JOIN AlbumArt a ON a.id = t.albumArtId WHERE t.path = ?;";
    const bool prepared = sqlite3_prepare_v2(handle, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK;
    if (prepared) {
        sqlite3_bind_text(stmt, 1, enginePath.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            // An older library keeps the image in the row; the hash beside
            // it is text and names no file.
            if (const void *image = sqlite3_column_blob(stmt, 1)) {
                std::string bytes(static_cast<const char *>(image), static_cast<size_t>(sqlite3_column_bytes(stmt, 1)));
                if (!engine::extensionForImage(bytes).empty()) {
                    found.bytes = std::move(bytes);
                }
            }
            const bool blobHash = sqlite3_column_type(stmt, 0) == SQLITE_BLOB;
            const void *blob = sqlite3_column_blob(stmt, 0);
            const int size = sqlite3_column_bytes(stmt, 0);
            if (found.bytes.empty() && blobHash && blob != nullptr && size == 20) {
                found.name = engine::artworkFileName(
                    std::span<const std::uint8_t>(static_cast<const std::uint8_t *>(blob), 20));
            }
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);
    return found;
}

std::string BackupArtworkLookup::findForTrack(const std::string &trackPath)
{
    if (trackPath.empty()) {
        return {};
    }
    const auto cached = m_foundByTrack.find(trackPath);
    if (cached != m_foundByTrack.end()) {
        return cached->second;
    }
    std::string bytes;
    for (const fs::path &archive : m_archives) {
        std::error_code ec;
        if (!fs::is_regular_file(archive, ec)) {
            continue;
        }
        TrackArtwork found = artworkForTrack(archive, trackPath);
        if (!found.bytes.empty()) {
            bytes = std::move(found.bytes);
            break;
        }
        if (found.name.empty()) {
            continue;
        }
        bytes = find(found.name);
        if (!bytes.empty()) {
            break;
        }
    }
    m_foundByTrack.emplace(trackPath, bytes);
    return bytes;
}

std::string BackupArtworkLookup::find(const std::string &name)
{
    if (name.empty()) {
        return {};
    }
    const auto cached = m_found.find(name);
    if (cached != m_found.end()) {
        return cached->second;
    }
    std::string bytes;
    for (const fs::path &archive : m_archives) {
        std::error_code ec;
        if (!fs::is_regular_file(archive, ec)) {
            continue;
        }
        try {
            PosixArchiveFile file(archive, PosixArchiveFile::OpenMode::ReadOnly);
            const Zip64Reader reader = Zip64Reader::open(file);
            for (const char *extension : {".jpg", ".jpeg", ".png"}) {
                // Archive paths are the stick's own, forward slashes and
                // all: the artwork directory sits where Engine keeps it.
                const auto entry = reader.findEntry("Engine Library/Artwork/" + name + extension);
                if (!entry) {
                    continue;
                }
                bytes = reader.readEntryToString(*entry);
                if (!bytes.empty()) {
                    break;
                }
            }
        } catch (const std::exception &) {
            // A backup that cannot be opened is one fewer place to look,
            // not a failure of the scan that asked.
            continue;
        }
        if (!bytes.empty()) {
            break;
        }
    }
    m_found.emplace(name, bytes);
    return bytes;
}

bool BackupArtworkLookup::has(const std::string &name)
{
    return !find(name).empty();
}

}  // namespace seabass::infrastructure::stick_backup
