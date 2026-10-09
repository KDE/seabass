// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/engine_playlists.hpp"

#include <filesystem>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

#include <djinterop/djinterop.hpp>
#include <sqlite3.h>

#include "infrastructure/engine/engine_track_rows.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::infrastructure::engine
{

namespace
{

constexpr const char *DanglingWhere =
    "FROM PlaylistEntity e JOIN Playlist p ON p.id = e.listId "
    "LEFT JOIN Track t ON t.id = e.trackId WHERE t.id IS NULL AND e.trackId > 0";

std::optional<djinterop::playlist> playlistAtPath(djinterop::database &db, const std::string &path)
{
    std::optional<djinterop::playlist> current;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!current) {
            for (const auto &root : db.root_playlists()) {
                if (root.name() == part) {
                    current = root;
                    break;
                }
            }
            if (!current) {
                return std::nullopt;
            }
        } else {
            current = current->sub_playlist_by_name(part);
            if (!current) {
                return std::nullopt;
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return current;
}

struct Resolved
{
    djinterop::database db;
    djinterop::playlist playlist;
    djinterop::track track;
};

Resolved resolve(const std::string &engineLibraryPath, const std::string &playlistPath, std::int64_t trackId)
{
    auto db = djinterop::engine::load_database(engineLibraryPath);
    auto playlist = playlistAtPath(db, playlistPath);
    if (!playlist) {
        throw std::runtime_error("engine: no playlist \"" + playlistPath + "\"");
    }
    auto track = db.track_by_id(trackId);
    if (!track) {
        throw std::runtime_error("engine: no track id=" + std::to_string(trackId));
    }
    return Resolved{db, *playlist, *track};
}

bool isMember(const djinterop::playlist &playlist, std::int64_t trackId)
{
    for (const auto &t : playlist.tracks()) {
        if (t.id() == trackId) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<DanglingPlaylistEntries> danglingPlaylistEntries(const std::string &engineLibraryPath)
{
    std::vector<DanglingPlaylistEntries> out;
    const std::string db = pathToUtf8(pathFromUtf8(engineLibraryPath) / "Database2" / "m.db");
    sqlite3 *handle = nullptr;
    if (sqlite3_open_v2(db.c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(handle);
        return out;
    }
    sqlite3_stmt *stmt = nullptr;
    const std::string sql = std::string("SELECT p.title, count(*) ") + DanglingWhere + " GROUP BY p.id ORDER BY p.title;";
    if (sqlite3_prepare_v2(handle, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const auto *title = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            out.push_back({title ? title : "", sqlite3_column_int(stmt, 1)});
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);
    return out;
}

int removeDanglingPlaylistEntries(const std::string &databaseFile)
{
    sqlite3 *handle = nullptr;
    if (sqlite3_open_v2(databaseFile.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        const std::string why = handle ? sqlite3_errmsg(handle) : "out of memory";
        sqlite3_close(handle);
        throw std::runtime_error("engine: could not open " + databaseFile + ": " + why);
    }
    const std::string sql = std::string("DELETE FROM PlaylistEntity WHERE id IN (SELECT e.id ") + DanglingWhere + ");";
    char *error = nullptr;
    if (sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string why = error ? error : "unknown error";
        sqlite3_free(error);
        sqlite3_close(handle);
        throw std::runtime_error("engine: could not remove dangling playlist entries: " + why);
    }
    const int removed = sqlite3_changes(handle);
    sqlite3_close(handle);
    return removed;
}

bool addToEnginePlaylist(const std::string &engineLibraryPath, const std::string &playlistPath, std::int64_t trackId)
{
    Resolved r = resolve(engineLibraryPath, playlistPath, trackId);
    if (isMember(r.playlist, trackId)) {
        return false;
    }
    r.playlist.add_track_back(r.track);
    if (!isMember(r.playlist, trackId)) {
        throw std::runtime_error("engine: track id=" + std::to_string(trackId) + " is not in \"" + playlistPath
                                 + "\" after adding it");
    }
    return true;
}

bool removeFromEnginePlaylist(const std::string &engineLibraryPath, const std::string &playlistPath,
                              std::int64_t trackId)
{
    Resolved r = resolve(engineLibraryPath, playlistPath, trackId);
    if (!isMember(r.playlist, trackId)) {
        return false;
    }
    r.playlist.remove_track(r.track);
    if (isMember(r.playlist, trackId)) {
        throw std::runtime_error("engine: track id=" + std::to_string(trackId) + " is still in \"" + playlistPath
                                 + "\" after removing it");
    }
    return true;
}

bool enginePlaylistExists(const std::string &engineLibraryPath, const std::string &playlistPath)
{
    try {
        auto db = djinterop::engine::load_database(engineLibraryPath);
        return playlistAtPath(db, playlistPath).has_value();
    } catch (const std::exception &) {
        return false;
    }
}

std::vector<std::int64_t> engineTrackIdsForFile(const std::string &databaseRoot, const std::string &realLibraryPath,
                                                const std::string &filePath)
{
    auto db = djinterop::engine::load_database(databaseRoot);
    std::error_code ec;
    const auto relative = std::filesystem::relative(pathFromUtf8(filePath), pathFromUtf8(realLibraryPath), ec);
    std::vector<std::int64_t> ids;
    if (ec) {
        return ids;
    }
    for (const auto &t : db.tracks_by_relative_path(pathToGenericUtf8(relative))) {
        ids.push_back(t.id());
    }
    return ids;
}

namespace
{

void countSpelled(const djinterop::playlist &pl, const std::string &prefix, const std::string &wanted, int &count)
{
    const std::string spelled = prefix.empty() ? pl.name() : prefix + "/" + pl.name();
    if (spelled == wanted) {
        ++count;
    }
    for (const auto &child : pl.children()) {
        countSpelled(child, spelled, wanted, count);
    }
}

}  // namespace

int enginePlaylistCountAtPath(const std::string &engineLibraryPath, const std::string &playlistPath)
{
    auto db = djinterop::engine::load_database(engineLibraryPath);
    int count = 0;
    for (const auto &root : db.root_playlists()) {
        countSpelled(root, "", playlistPath, count);
    }
    return count;
}

namespace
{

void collectPlaylistTracks(const djinterop::playlist &pl, std::set<std::int64_t> &ids)
{
    for (const auto &t : pl.tracks()) {
        ids.insert(t.id());
    }
    for (const auto &child : pl.children()) {
        collectPlaylistTracks(child, ids);
    }
}

std::set<std::int64_t> tracksInPlaylists(djinterop::database &db)
{
    std::set<std::int64_t> ids;
    for (const auto &root : db.root_playlists()) {
        collectPlaylistTracks(root, ids);
    }
    return ids;
}

}  // namespace

std::vector<std::int64_t> engineTracksInAnyPlaylist(const std::string &engineLibraryPath)
{
    auto db = djinterop::engine::load_database(engineLibraryPath);
    const auto ids = tracksInPlaylists(db);
    return {ids.begin(), ids.end()};
}

int removeEngineTracks(const std::string &engineLibraryPath, const std::vector<std::int64_t> &trackIds)
{
    std::vector<std::int64_t> present;
    {
        auto db = djinterop::engine::load_database(engineLibraryPath);
        const auto listed = tracksInPlaylists(db);
        // Every id checked before anything is written: Delete Tracks
        // leaves a listed track alone, and this guard backs that rule even
        // for a caller that did not apply it. removeEngineTrackRows itself
        // would take the playlist entries along.
        for (const std::int64_t id : trackIds) {
            if (listed.count(id)) {
                throw std::runtime_error("engine: track id=" + std::to_string(id) + " is in a playlist; not removing it");
            }
        }
        for (const std::int64_t id : trackIds) {
            if (db.track_by_id(id)) {
                present.push_back(id);
            }
        }
    }
    if (present.empty()) {
        return 0;
    }
    std::string error;
    const int removed =
        removeEngineTrackRows(pathToUtf8(pathFromUtf8(engineLibraryPath) / "Database2" / "m.db"), present, &error);
    if (removed < 0) {
        throw std::runtime_error("engine: could not remove tracks: " + error);
    }
    return removed;
}

int deleteEnginePlaylist(const std::string &engineLibraryPath, const std::string &playlistPath)
{
    auto db = djinterop::engine::load_database(engineLibraryPath);
    const auto top = playlistAtPath(db, playlistPath);
    if (!top) {
        return 0;
    }
    // Children before parents, so each playlist is removed after
    // everything below it.
    std::vector<djinterop::playlist> order;
    std::vector<djinterop::playlist> pending{*top};
    while (!pending.empty()) {
        djinterop::playlist pl = pending.back();
        pending.pop_back();
        order.push_back(pl);
        for (const auto &child : pl.children()) {
            pending.push_back(child);
        }
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        it->clear_tracks();
        db.remove_playlist(*it);
    }
    if (playlistAtPath(db, playlistPath)) {
        throw std::runtime_error("engine: \"" + playlistPath + "\" is still there after deleting it");
    }
    // No entry may be left naming a playlist that is gone.
    const std::string file = pathToUtf8(pathFromUtf8(engineLibraryPath) / "Database2" / "m.db");
    sqlite3 *handle = nullptr;
    if (sqlite3_open_v2(file.c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt *stmt = nullptr;
        int orphans = 0;
        if (sqlite3_prepare_v2(handle, "SELECT count(*) FROM PlaylistEntity WHERE listId NOT IN (SELECT id FROM Playlist);", -1,
                               &stmt, nullptr)
                == SQLITE_OK
            && sqlite3_step(stmt) == SQLITE_ROW) {
            orphans = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
        sqlite3_close(handle);
        if (orphans > 0) {
            throw std::runtime_error("engine: " + std::to_string(orphans) + " playlist entries left without a playlist");
        }
    } else {
        sqlite3_close(handle);
    }
    return static_cast<int>(order.size());
}

}  // namespace seabass::infrastructure::engine
