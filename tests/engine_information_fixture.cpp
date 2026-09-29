// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "engine_information_fixture.hpp"

#include <sqlite3.h>

#include <stdexcept>
#include <string>

#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::testing
{

void createEngineInformation(const std::filesystem::path &databaseFile, std::int64_t counter, int rowId)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open(seabass::pathToUtf8(databaseFile).c_str(), &db) != SQLITE_OK) {
        throw std::runtime_error("cannot create " + seabass::pathToUtf8(databaseFile));
    }
    const std::string sql = "CREATE TABLE Information (id INTEGER PRIMARY KEY, lastRekordBoxLibraryImportReadCounter "
                            "INTEGER); INSERT INTO Information VALUES ("
        + std::to_string(rowId) + ", " + std::to_string(counter) + ");";
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("cannot write Information in " + seabass::pathToUtf8(databaseFile));
    }
}

std::int64_t readEngineImportCounter(const std::filesystem::path &databaseFile)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(seabass::pathToUtf8(databaseFile).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *stmt = nullptr;
    std::int64_t value = -1;
    if (sqlite3_prepare_v2(db, "SELECT lastRekordBoxLibraryImportReadCounter FROM Information", -1, &stmt, nullptr)
            == SQLITE_OK
        && sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return value;
}

void createEngineArtworkTables(const std::filesystem::path &databaseFile, const std::vector<std::int64_t> &trackIds)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open(seabass::pathToUtf8(databaseFile).c_str(), &db) != SQLITE_OK) {
        throw std::runtime_error("cannot create " + seabass::pathToUtf8(databaseFile));
    }
    std::string sql = "CREATE TABLE AlbumArt (id INTEGER PRIMARY KEY AUTOINCREMENT, hash TEXT, albumArt BLOB);"
                      "CREATE TABLE Track (id INTEGER PRIMARY KEY, title TEXT, artist TEXT, albumArtId INTEGER);";
    for (const std::int64_t id : trackIds) {
        sql += "INSERT INTO Track (id, title, artist, albumArtId) VALUES (" + std::to_string(id) + ", 't', 'a', NULL);";
    }
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("cannot write the artwork tables in " + seabass::pathToUtf8(databaseFile));
    }
}

void setEngineDatabaseArtwork(const std::filesystem::path &databaseFile, std::int64_t rowId, const std::string &hash,
                              const std::string &image, const std::vector<std::int64_t> &trackIds)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open(seabass::pathToUtf8(databaseFile).c_str(), &db) != SQLITE_OK) {
        throw std::runtime_error("cannot open " + seabass::pathToUtf8(databaseFile));
    }
    sqlite3_stmt *stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO AlbumArt (id, hash, albumArt) VALUES (?, ?, ?);", -1, &stmt,
                                 nullptr)
        == SQLITE_OK;
    if (ok) {
        sqlite3_bind_int64(stmt, 1, rowId);
        sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 3, image.data(), static_cast<int>(image.size()), SQLITE_TRANSIENT);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    for (const std::int64_t track : trackIds) {
        const std::string sql = "UPDATE Track SET albumArtId = " + std::to_string(rowId) + " WHERE id = "
            + std::to_string(track) + ";";
        ok = ok && sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
    }
    sqlite3_close(db);
    if (!ok) {
        throw std::runtime_error("cannot write the artwork row in " + seabass::pathToUtf8(databaseFile));
    }
}

std::string engineAlbumArtImage(const std::filesystem::path &databaseFile, std::int64_t rowId)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(seabass::pathToUtf8(databaseFile).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return {};
    }
    sqlite3_stmt *stmt = nullptr;
    std::string image;
    if (sqlite3_prepare_v2(db, "SELECT albumArt FROM AlbumArt WHERE id = ?", -1, &stmt, nullptr) == SQLITE_OK
        && sqlite3_bind_int64(stmt, 1, rowId) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        if (const void *blob = sqlite3_column_blob(stmt, 0)) {
            image.assign(static_cast<const char *>(blob), static_cast<size_t>(sqlite3_column_bytes(stmt, 0)));
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return image;
}

std::int64_t engineTrackAlbumArtId(const std::filesystem::path &databaseFile, std::int64_t trackId)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(seabass::pathToUtf8(databaseFile).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *stmt = nullptr;
    std::int64_t id = -1;
    if (sqlite3_prepare_v2(db, "SELECT albumArtId FROM Track WHERE id = ?", -1, &stmt, nullptr) == SQLITE_OK
        && sqlite3_bind_int64(stmt, 1, trackId) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        id = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return id;
}

std::string engineTrackArtworkHash(const std::filesystem::path &databaseFile, std::int64_t trackId)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(seabass::pathToUtf8(databaseFile).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return {};
    }
    sqlite3_stmt *stmt = nullptr;
    std::string hash;
    if (sqlite3_prepare_v2(db, "SELECT AlbumArt.hash FROM Track JOIN AlbumArt ON AlbumArt.id = Track.albumArtId "
                               "WHERE Track.id = ?", -1, &stmt, nullptr)
            == SQLITE_OK
        && sqlite3_bind_int64(stmt, 1, trackId) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW
        && sqlite3_column_text(stmt, 0) != nullptr) {
        hash = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return hash;
}

}  // namespace seabass::testing
