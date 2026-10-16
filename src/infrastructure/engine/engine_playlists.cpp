// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/engine_playlists.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <variant>
#include <vector>

#include <djinterop/djinterop.hpp>
#include <sqlite3.h>

#include "infrastructure/engine/engine_track_rows.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/utc_timestamp.hpp"

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

// Not libdjinterop's playlist::add_track_back: it gives every entry the
// library's own uuid, which a player cannot resolve where the tracks name
// another library as their origin (see entityUuidFor). Appended the way the sync's inserts
// are, with the track's.
bool addToEnginePlaylist(const std::string &engineLibraryPath, const std::string &playlistPath, std::int64_t trackId)
{
    return insertIntoEnginePlaylist(engineLibraryPath, playlistPath, trackId, std::nullopt);
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

void collectPlaylists(const djinterop::playlist &pl, const std::string &prefix,
                      std::vector<domain::EnginePlaylistInfo> &out)
{
    const std::string spelled = prefix.empty() ? pl.name() : prefix + "/" + pl.name();
    const auto children = pl.children();
    out.push_back(domain::EnginePlaylistInfo{spelled, !children.empty(), 1});
    for (const auto &child : children) {
        collectPlaylists(child, spelled, out);
    }
}

}  // namespace

std::vector<domain::EnginePlaylistInfo> listEnginePlaylists(const std::string &engineLibraryPath)
{
    auto db = djinterop::engine::load_database(engineLibraryPath);
    std::vector<domain::EnginePlaylistInfo> out;
    for (const auto &root : db.root_playlists()) {
        collectPlaylists(root, "", out);
    }
    std::map<std::string, int> spellings;
    for (const auto &playlist : out) {
        ++spellings[playlist.path];
    }
    for (auto &playlist : out) {
        playlist.countAtPath = spellings[playlist.path];
    }
    return out;
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

namespace
{

// "A/B/C" as its titles. An empty path or an empty title ("A//B", a
// leading or trailing "/") names nothing and is refused.
std::vector<std::string> splitPlaylistPath(const std::string &path)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t slash = path.find('/', start);
        parts.push_back(path.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (parts.back().empty()) {
            throw std::runtime_error("engine: \"" + path + "\" is not a playlist path (an empty title)");
        }
        if (slash == std::string::npos) {
            return parts;
        }
        start = slash + 1;
    }
}

std::string joinPlaylistPath(const std::vector<std::string> &parts, size_t count)
{
    std::string out;
    for (size_t i = 0; i < count; ++i) {
        out += (i ? "/" : "") + parts[i];
    }
    return out;
}

void refuseUnwritableTitle(const std::string &title, const std::string &path)
{
    // libdjinterop's playlist_table refuses the same: Engine joins titles
    // with ';' in its PlaylistPath view.
    if (title.find(';') != std::string::npos) {
        throw std::runtime_error("engine: \"" + path + "\": Engine does not take a ';' in a playlist title");
    }
}

// One read-write connection to a library's m.db, with one transaction
// that rolls back unless committed.
class PlaylistWriter
{
public:
    explicit PlaylistWriter(const std::string &engineLibraryPath)
        : m_file(pathToUtf8(pathFromUtf8(engineLibraryPath) / "Database2" / "m.db"))
    {
        if (!std::filesystem::is_regular_file(pathFromUtf8(m_file))) {
            throw std::runtime_error("engine: no Engine library at " + engineLibraryPath);
        }
        if (sqlite3_open_v2(m_file.c_str(), &m_handle, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
            const std::string why = m_handle ? sqlite3_errmsg(m_handle) : "out of memory";
            sqlite3_close(m_handle);
            m_handle = nullptr;
            throw std::runtime_error("engine: could not open " + m_file + ": " + why);
        }
        sqlite3_busy_timeout(m_handle, 5000);
        exec("BEGIN IMMEDIATE;");
        m_open = true;
    }
    ~PlaylistWriter()
    {
        if (m_open) {
            sqlite3_exec(m_handle, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
        sqlite3_close(m_handle);
    }
    PlaylistWriter(const PlaylistWriter &) = delete;
    PlaylistWriter &operator=(const PlaylistWriter &) = delete;

    void commit()
    {
        exec("COMMIT;");
        m_open = false;
    }

    void exec(const std::string &sql)
    {
        char *error = nullptr;
        if (sqlite3_exec(m_handle, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string why = error ? error : sqlite3_errmsg(m_handle);
            sqlite3_free(error);
            throw std::runtime_error("engine: " + why);
        }
    }

    // Runs `sql` with `binds` (each an integer or a text), calling `row`
    // for every result row. Returns how many rows the statement changed.
    using Bind = std::variant<std::int64_t, std::string>;
    int run(const std::string &sql, const std::vector<Bind> &binds,
            const std::function<void(sqlite3_stmt *)> &row = nullptr)
    {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(m_handle, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            const std::string why = sqlite3_errmsg(m_handle);
            sqlite3_finalize(stmt);
            throw std::runtime_error("engine: " + why);
        }
        for (size_t i = 0; i < binds.size(); ++i) {
            const int at = static_cast<int>(i + 1);
            if (const auto *n = std::get_if<std::int64_t>(&binds[i])) {
                sqlite3_bind_int64(stmt, at, *n);
            } else {
                const auto &s = std::get<std::string>(binds[i]);
                sqlite3_bind_text(stmt, at, s.c_str(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
            }
        }
        int step = SQLITE_ROW;
        while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
            if (row) {
                row(stmt);
            }
        }
        const std::string why = sqlite3_errmsg(m_handle);
        sqlite3_finalize(stmt);
        if (step != SQLITE_DONE) {
            throw std::runtime_error("engine: " + why);
        }
        return sqlite3_changes(m_handle);
    }

    std::int64_t lastInsertId() const { return sqlite3_last_insert_rowid(m_handle); }

private:
    std::string m_file;
    sqlite3 *m_handle = nullptr;
    bool m_open = false;
};

struct PlaylistRow
{
    std::string title;
    std::int64_t parent = 0;
    std::int64_t next = 0;
};

// Every Playlist row with the path the reader spells for it.
struct PlaylistTree
{
    std::map<std::int64_t, PlaylistRow> rows;
    std::map<std::int64_t, std::string> spelled;

    std::vector<std::int64_t> at(const std::string &path) const
    {
        std::vector<std::int64_t> ids;
        for (const auto &[id, s] : spelled) {
            if (s == path) {
                ids.push_back(id);
            }
        }
        return ids;
    }

    // Whether `id` is `ancestor` or lies below it.
    bool within(std::int64_t id, std::int64_t ancestor) const
    {
        for (size_t steps = 0; id != 0 && steps <= rows.size(); ++steps) {
            if (id == ancestor) {
                return true;
            }
            const auto it = rows.find(id);
            if (it == rows.end()) {
                return false;
            }
            id = it->second.parent;
        }
        return false;
    }
};

PlaylistTree readPlaylistTree(PlaylistWriter &db)
{
    PlaylistTree tree;
    db.run("SELECT id, title, parentListId, nextListId FROM Playlist;", {}, [&](sqlite3_stmt *s) {
        const auto *title = reinterpret_cast<const char *>(sqlite3_column_text(s, 1));
        tree.rows[sqlite3_column_int64(s, 0)] =
            PlaylistRow{title ? title : "", sqlite3_column_int64(s, 2), sqlite3_column_int64(s, 3)};
    });
    for (const auto &[id, row] : tree.rows) {
        std::string path = row.title;
        std::int64_t parent = row.parent;
        for (size_t steps = 0; parent != 0; ++steps) {
            const auto it = tree.rows.find(parent);
            if (it == tree.rows.end() || steps > tree.rows.size()) {
                throw std::runtime_error("engine: playlist id=" + std::to_string(id)
                                         + " has a parent that does not exist or loops");
            }
            path = it->second.title + "/" + path;
            parent = it->second.parent;
        }
        tree.spelled.emplace(id, path);
    }
    return tree;
}

// The one playlist spelling `path`, or a throw saying why there is not one.
std::int64_t uniquePlaylist(const PlaylistTree &tree, const std::string &path)
{
    const auto ids = tree.at(path);
    if (ids.empty()) {
        throw std::runtime_error("engine: no playlist \"" + path + "\"");
    }
    if (ids.size() > 1) {
        throw std::runtime_error("engine: " + std::to_string(ids.size()) + " playlists spell \"" + path
                                 + "\"; refusing to pick one");
    }
    return ids.front();
}

// A linked list as Engine keeps one: each row's id and the id of the row
// after it, 0 after the last. The ids from the head in order when the
// walk is whole (one head, no loop, no pointer outside the list, every
// row reached), nullopt when it is not.
std::optional<std::vector<std::int64_t>> walkChain(const std::map<std::int64_t, std::int64_t> &nextOf)
{
    std::vector<std::int64_t> order;
    if (nextOf.empty()) {
        return order;
    }
    std::set<std::int64_t> pointedAt;
    for (const auto &[id, next] : nextOf) {
        pointedAt.insert(next);
    }
    std::optional<std::int64_t> head;
    for (const auto &[id, next] : nextOf) {
        if (!pointedAt.count(id)) {
            if (head) {
                return std::nullopt;
            }
            head = id;
        }
    }
    if (!head) {
        return std::nullopt;
    }
    std::set<std::int64_t> seen;
    for (std::int64_t at = *head; at != 0;) {
        const auto it = nextOf.find(at);
        if (it == nextOf.end() || !seen.insert(at).second) {
            return std::nullopt;
        }
        order.push_back(at);
        at = it->second;
    }
    if (order.size() != nextOf.size()) {
        return std::nullopt;
    }
    return order;
}

// The children of `parentId` (0: the root playlists) in their order.
// Throws when their nextListId chain does not walk.
std::vector<std::int64_t> siblingsInOrder(const PlaylistTree &tree, std::int64_t parentId)
{
    std::map<std::int64_t, std::int64_t> nextOf;
    for (const auto &[id, row] : tree.rows) {
        if (row.parent == parentId) {
            nextOf[id] = row.next;
        }
    }
    auto order = walkChain(nextOf);
    if (!order) {
        throw std::runtime_error("engine: the playlists under parent id=" + std::to_string(parentId)
                                 + " do not form one chain");
    }
    return *order;
}

struct ListEntry
{
    std::int64_t trackId;
    std::int64_t next;
};

std::map<std::int64_t, ListEntry> listEntries(PlaylistWriter &db, std::int64_t listId)
{
    std::map<std::int64_t, ListEntry> out;
    db.run("SELECT id, trackId, nextEntityId FROM PlaylistEntity WHERE listId = ?;", {listId}, [&](sqlite3_stmt *s) {
        out[sqlite3_column_int64(s, 0)] = ListEntry{sqlite3_column_int64(s, 1), sqlite3_column_int64(s, 2)};
    });
    return out;
}

// A list's track ids in chain order, or nullopt when the chain is broken.
std::optional<std::vector<std::int64_t>> listInOrder(const std::map<std::int64_t, ListEntry> &entries)
{
    std::map<std::int64_t, std::int64_t> nextOf;
    for (const auto &[id, e] : entries) {
        nextOf[id] = e.next;
    }
    const auto order = walkChain(nextOf);
    if (!order) {
        return std::nullopt;
    }
    std::vector<std::int64_t> tracks;
    tracks.reserve(order->size());
    for (const std::int64_t id : *order) {
        tracks.push_back(entries.at(id).trackId);
    }
    return tracks;
}

// lastEditTime as libdjinterop writes it (date::format("%F %T") of a
// system_clock time: UTC, nanoseconds).
std::string engineEditTime()
{
    const auto now = std::chrono::system_clock::now();
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           now - std::chrono::floor<std::chrono::seconds>(now))
                           .count();
    std::string fraction = std::to_string(nanos);
    fraction.insert(0, 9 - std::min<size_t>(9, fraction.size()), '0');
    return utcTimestamp(now, "%Y-%m-%d %H:%M:%S") + "." + fraction;
}

}  // namespace

std::int64_t createEnginePlaylist(const std::string &engineLibraryPath, const std::string &playlistPath)
{
    const auto parts = splitPlaylistPath(playlistPath);
    for (const auto &title : parts) {
        refuseUnwritableTitle(title, playlistPath);
    }
    PlaylistWriter db(engineLibraryPath);
    const PlaylistTree tree = readPlaylistTree(db);

    // Everything checked before the first row is written.
    std::int64_t parent = 0;
    size_t firstMissing = parts.size();
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string prefix = joinPlaylistPath(parts, i + 1);
        const auto ids = tree.at(prefix);
        if (i + 1 == parts.size() && !ids.empty()) {
            throw std::runtime_error("engine: \"" + playlistPath + "\" already exists");
        }
        if (firstMissing < parts.size()) {
            // An ancestor is about to be created, yet something already
            // reads as this prefix (a title with a "/" in it): afterwards
            // two playlists would spell it.
            if (!ids.empty()) {
                throw std::runtime_error("engine: creating \"" + playlistPath + "\" would make \"" + prefix
                                         + "\" name two playlists");
            }
            continue;
        }
        if (ids.size() > 1) {
            throw std::runtime_error("engine: " + std::to_string(ids.size()) + " playlists spell \"" + prefix
                                     + "\"; refusing to create below it");
        }
        if (ids.empty()) {
            firstMissing = i;
        } else {
            parent = ids.front();
        }
    }

    // The row create_root_playlist and create_sub_playlist insert:
    // nextListId 0 makes it the last of its siblings, and Engine's insert
    // triggers point the previous last one at it.
    const std::string editTime = engineEditTime();
    std::vector<std::int64_t> created;
    for (size_t i = firstMissing; i < parts.size(); ++i) {
        db.run("INSERT INTO Playlist (title, parentListId, isPersisted, nextListId, lastEditTime, "
               "isExplicitlyExported) VALUES (?, ?, 1, 0, ?, 1);",
               {parts[i], parent, editTime});
        parent = db.lastInsertId();
        created.push_back(parent);
    }

    const PlaylistTree after = readPlaylistTree(db);
    for (size_t i = firstMissing; i < parts.size(); ++i) {
        const std::string prefix = joinPlaylistPath(parts, i + 1);
        const auto ids = after.at(prefix);
        if (ids.size() != 1 || ids.front() != created[i - firstMissing]) {
            throw std::runtime_error("engine: \"" + prefix + "\" does not read back as the playlist just created");
        }
        const auto siblings = siblingsInOrder(after, after.rows.at(ids.front()).parent);
        if (siblings.empty() || siblings.back() != ids.front()) {
            throw std::runtime_error("engine: \"" + prefix + "\" is not last among its siblings after creating it");
        }
    }
    db.commit();
    return created.back();
}

bool renameEnginePlaylist(const std::string &engineLibraryPath, const std::string &fromPath, const std::string &toPath)
{
    splitPlaylistPath(fromPath);
    const auto toParts = splitPlaylistPath(toPath);
    refuseUnwritableTitle(toParts.back(), toPath);
    PlaylistWriter db(engineLibraryPath);
    const PlaylistTree tree = readPlaylistTree(db);

    const std::int64_t subject = uniquePlaylist(tree, fromPath);
    if (fromPath == toPath) {
        return false;
    }
    if (!tree.at(toPath).empty()) {
        throw std::runtime_error("engine: cannot rename \"" + fromPath + "\": \"" + toPath + "\" already exists");
    }
    std::int64_t newParent = 0;
    if (toParts.size() > 1) {
        newParent = uniquePlaylist(tree, joinPlaylistPath(toParts, toParts.size() - 1));
        if (tree.within(newParent, subject)) {
            throw std::runtime_error("engine: cannot move \"" + fromPath + "\" into itself (\"" + toPath + "\")");
        }
    }
    // Its children are respelled under the new path; none of those
    // spellings may already belong to a playlist outside it.
    for (const auto &[id, spelled] : tree.spelled) {
        if (id == subject || !tree.within(id, subject)) {
            continue;
        }
        const std::string respelled = toPath + spelled.substr(fromPath.size());
        for (const std::int64_t other : tree.at(respelled)) {
            if (!tree.within(other, subject)) {
                throw std::runtime_error("engine: renaming \"" + fromPath + "\" to \"" + toPath + "\" would make \""
                                         + respelled + "\" name two playlists");
            }
        }
    }

    const auto entriesBefore = listInOrder(listEntries(db, subject));
    const std::int64_t oldParent = tree.rows.at(subject).parent;
    const std::int64_t oldNext = tree.rows.at(subject).next;
    const std::string editTime = engineEditTime();
    if (newParent == oldParent) {
        db.run("UPDATE Playlist SET title = ?, lastEditTime = ? WHERE id = ?;", {toParts.back(), editTime, subject});
    } else {
        // libdjinterop's playlist_table::update for a changed parent, with
        // nextListId 0 (Engine has no UPDATE trigger for this): detach the
        // row (its nextListId made negative, clear of the unique
        // (parentListId, nextListId)), point its old predecessor at its
        // old successor, point the new parent's last child at it, then
        // move it with nextListId 0.
        db.run("UPDATE Playlist SET nextListId = -(1 + nextListId) WHERE id = ?;", {subject});
        db.run("UPDATE Playlist SET nextListId = ? WHERE nextListId = ? AND parentListId = ?;",
               {oldNext, subject, oldParent});
        db.run("UPDATE Playlist SET nextListId = ? WHERE nextListId = 0 AND parentListId = ?;", {subject, newParent});
        db.run("UPDATE Playlist SET title = ?, parentListId = ?, nextListId = 0, lastEditTime = ? WHERE id = ?;",
               {toParts.back(), newParent, editTime, subject});
    }

    const PlaylistTree after = readPlaylistTree(db);
    const auto at = after.at(toPath);
    if (at.size() != 1 || at.front() != subject || !after.at(fromPath).empty()) {
        throw std::runtime_error("engine: \"" + fromPath + "\" does not read back as \"" + toPath + "\" after renaming it");
    }
    const auto newSiblings = siblingsInOrder(after, newParent);
    siblingsInOrder(after, oldParent);
    if (newParent != oldParent && (newSiblings.empty() || newSiblings.back() != subject)) {
        throw std::runtime_error("engine: \"" + toPath + "\" is not last in its new folder after moving it");
    }
    if (listInOrder(listEntries(db, subject)) != entriesBefore) {
        throw std::runtime_error("engine: the entries of \"" + toPath + "\" changed while renaming it");
    }
    db.commit();
    return true;
}

namespace
{

// The databaseUuid a playlist entry for `trackId` carries: the track's
// originDatabaseUuid when it has one, else the library's own (the one
// Information row's). The player resolves an entry by (databaseUuid,
// trackId) against the tracks' (originDatabaseUuid, originTrackId), and
// the two uuids need not be the same. WHALESHARK3 is a Seabass restore of
// WHALESHARK: its tracks carry WHALESHARK's library uuid (fb65ee77...) as
// their origin, the restore kept Information.uuid as well, and something
// since (most likely the Prime 4, meeting a second stick with a library
// uuid it already knew) renewed the stick's Information.uuid to another
// (52f6b8cd...) while the tracks' origin stayed. An entry written with the
// stick's uuid was then invisible on the player: "50th Opening" showed
// empty, its 8 entries and the 112 others Seabass had written carrying
// 52f6b8cd, against 1570 entries the player and Engine DJ wrote with the
// tracks' fb65ee77. In a library made on the stick the two are the same.
//
// Engine's own triggers fill an empty origin with the library's uuid and
// the row's id (trigger_after_insert_Track_fix_origin and its update
// twin), so a track naming no origin is only met in a library without
// them; the fallback is for that.
//
// trackId stays the Track row's id, as libdjinterop and Engine DJ write
// it. Whether the player matches it against id or originTrackId the data
// cannot tell: on WHALESHARK3 the two are equal for all 1359 tracks.
std::string entityUuidFor(PlaylistWriter &db, std::int64_t trackId)
{
    bool hasOrigin = false;
    db.run("PRAGMA table_info(Track);", {}, [&](sqlite3_stmt *s) {
        const auto *name = reinterpret_cast<const char *>(sqlite3_column_text(s, 1));
        hasOrigin = hasOrigin || (name != nullptr && std::string(name) == "originDatabaseUuid");
    });
    if (hasOrigin) {
        std::string origin;
        db.run("SELECT originDatabaseUuid FROM Track WHERE id = ?;", {trackId}, [&](sqlite3_stmt *s) {
            const auto *u = reinterpret_cast<const char *>(sqlite3_column_text(s, 0));
            origin = u ? u : "";
        });
        if (!origin.empty()) {
            return origin;
        }
    }
    std::vector<std::string> uuids;
    db.run("SELECT uuid FROM Information;", {}, [&](sqlite3_stmt *s) {
        const auto *u = reinterpret_cast<const char *>(sqlite3_column_text(s, 0));
        uuids.emplace_back(u ? u : "");
    });
    if (uuids.size() != 1 || uuids.front().empty()) {
        throw std::runtime_error("engine: the library has " + std::to_string(uuids.size())
                                 + " Information rows, not one with a uuid, and track id=" + std::to_string(trackId)
                                 + " names no origin; refusing to add playlist entries");
    }
    return uuids.front();
}

// Where insertEntry puts a track: after an anchor (appended when there is
// none or it is not a member), or first.
enum class Placement { AfterAnchor, AtStart };

bool insertEntry(const std::string &engineLibraryPath, const std::string &playlistPath, std::int64_t trackId,
                 Placement placement, std::optional<std::int64_t> afterTrackId)
{
    splitPlaylistPath(playlistPath);
    PlaylistWriter db(engineLibraryPath);
    const std::int64_t listId = uniquePlaylist(readPlaylistTree(db), playlistPath);
    bool trackExists = false;
    db.run("SELECT 1 FROM Track WHERE id = ?;", {trackId}, [&](sqlite3_stmt *) { trackExists = true; });
    if (!trackExists) {
        throw std::runtime_error("engine: no track id=" + std::to_string(trackId));
    }
    const std::string entryUuid = entityUuidFor(db, trackId);

    const auto entries = listEntries(db, listId);
    const auto before = listInOrder(entries);
    if (!before) {
        throw std::runtime_error("engine: the entries of \"" + playlistPath
                                 + "\" do not form one chain; refusing to insert into it");
    }
    for (const auto &[id, e] : entries) {
        if (e.trackId == trackId) {
            return false;
        }
    }
    // The anchor's entry when the anchor is in the list; otherwise the
    // track is appended.
    std::optional<ListEntry> anchor;
    if (placement == Placement::AfterAnchor && afterTrackId) {
        for (const auto &[id, e] : entries) {
            if (e.trackId == *afterTrackId) {
                anchor = e;
                break;
            }
        }
    }
    // First: the new entry points at the head, the entry no other entry
    // points at (0 in an empty list, which is an append). Nothing pointed
    // at the head, so the UPDATE below finds no row, which is right: the
    // new entry is the head now.
    std::int64_t head = 0;
    if (placement == Placement::AtStart) {
        std::set<std::int64_t> pointedAt;
        for (const auto &[id, e] : entries) {
            pointedAt.insert(e.next);
        }
        for (const auto &[id, e] : entries) {
            if (!pointedAt.count(id)) {
                head = id;
                break;
            }
        }
    }
    // playlist_entity_table::add's two statements: the new entry takes
    // the anchor's old next (0 to append, the head to go first), and the
    // one entry that pointed there (the anchor, or the last entry; none
    // for the head) now points at the new one.
    const std::int64_t next = placement == Placement::AtStart ? head : anchor ? anchor->next : 0;
    db.run("INSERT INTO PlaylistEntity (listId, trackId, databaseUuid, nextEntityId, membershipReference) "
           "VALUES (?, ?, ?, ?, 0);",
           {listId, trackId, entryUuid, next});
    const std::int64_t added = db.lastInsertId();
    db.run("UPDATE PlaylistEntity SET nextEntityId = ? WHERE listId = ? AND nextEntityId = ? AND id <> ?;",
           {added, listId, next, added});

    std::vector<std::int64_t> expected = *before;
    if (placement == Placement::AtStart) {
        expected.insert(expected.begin(), trackId);
    } else if (anchor) {
        expected.insert(std::find(expected.begin(), expected.end(), *afterTrackId) + 1, trackId);
    } else {
        expected.push_back(trackId);
    }
    if (listInOrder(listEntries(db, listId)) != expected) {
        throw std::runtime_error("engine: \"" + playlistPath
                                 + "\" does not read back in the order asked after adding track id="
                                 + std::to_string(trackId));
    }
    db.commit();
    return true;
}

}  // namespace

bool insertIntoEnginePlaylist(const std::string &engineLibraryPath, const std::string &playlistPath,
                              std::int64_t trackId, std::optional<std::int64_t> afterTrackId)
{
    return insertEntry(engineLibraryPath, playlistPath, trackId, Placement::AfterAnchor, afterTrackId);
}

bool insertAtStartOfEnginePlaylist(const std::string &engineLibraryPath, const std::string &playlistPath,
                                   std::int64_t trackId)
{
    return insertEntry(engineLibraryPath, playlistPath, trackId, Placement::AtStart, std::nullopt);
}

bool removeFromEnginePlaylist(const std::string &engineLibraryPath, const std::string &playlistPath,
                              std::int64_t trackId)
{
    splitPlaylistPath(playlistPath);
    PlaylistWriter db(engineLibraryPath);
    const std::int64_t listId = uniquePlaylist(readPlaylistTree(db), playlistPath);
    bool trackExists = false;
    db.run("SELECT 1 FROM Track WHERE id = ?;", {trackId}, [&](sqlite3_stmt *) { trackExists = true; });
    if (!trackExists) {
        throw std::runtime_error("engine: no track id=" + std::to_string(trackId));
    }

    const auto before = listInOrder(listEntries(db, listId));
    if (!before) {
        throw std::runtime_error("engine: the entries of \"" + playlistPath
                                 + "\" do not form one chain; refusing to remove from it");
    }
    std::vector<std::int64_t> expected = *before;
    std::erase(expected, trackId);
    if (expected.size() == before->size()) {
        return false;
    }
    // By the entry's track, not its own id (see the header). Engine's
    // trigger_before_delete_PlaylistEntity points the entry before each
    // deleted one at the entry after it.
    db.run("DELETE FROM PlaylistEntity WHERE listId = ? AND trackId = ?;", {listId, trackId});

    const auto after = listInOrder(listEntries(db, listId));
    if (!after) {
        throw std::runtime_error("engine: the entries of \"" + playlistPath
                                 + "\" do not form one chain after removing track id=" + std::to_string(trackId));
    }
    if (std::find(after->begin(), after->end(), trackId) != after->end()) {
        throw std::runtime_error("engine: track id=" + std::to_string(trackId) + " is still in \"" + playlistPath
                                 + "\" after removing it");
    }
    if (*after != expected) {
        throw std::runtime_error("engine: \"" + playlistPath
                                 + "\" does not read back in its old order after removing track id="
                                 + std::to_string(trackId));
    }
    db.commit();
    return true;
}

std::vector<std::int64_t> engineTrackIdsInPlaylist(const std::string &engineLibraryPath, const std::string &playlistPath)
{
    splitPlaylistPath(playlistPath);
    PlaylistWriter db(engineLibraryPath);
    const std::int64_t listId = uniquePlaylist(readPlaylistTree(db), playlistPath);
    const auto order = listInOrder(listEntries(db, listId));
    if (!order) {
        throw std::runtime_error("engine: the entries of \"" + playlistPath + "\" do not form one chain");
    }
    return *order;
}

bool moveInEnginePlaylist(const std::string &engineLibraryPath, const std::string &playlistPath, std::int64_t trackId,
                          std::optional<std::int64_t> afterTrackId)
{
    splitPlaylistPath(playlistPath);
    PlaylistWriter db(engineLibraryPath);
    const std::int64_t listId = uniquePlaylist(readPlaylistTree(db), playlistPath);
    const auto entries = listEntries(db, listId);
    const auto before = listInOrder(entries);
    if (!before) {
        throw std::runtime_error("engine: the entries of \"" + playlistPath
                                 + "\" do not form one chain; refusing to move within it");
    }
    std::optional<std::int64_t> subject;
    int held = 0;
    for (const auto &[id, e] : entries) {
        if (e.trackId == trackId) {
            subject = id;
            ++held;
        }
    }
    if (held != 1) {
        throw std::runtime_error("engine: \"" + playlistPath + "\" holds track id=" + std::to_string(trackId) + " "
                                 + std::to_string(held) + " times, not once; refusing to move it");
    }
    if (afterTrackId && (*afterTrackId == trackId || std::count(before->begin(), before->end(), *afterTrackId) != 1)) {
        throw std::runtime_error("engine: track id=" + std::to_string(*afterTrackId) + " is no anchor in \""
                                 + playlistPath + "\" to move track id=" + std::to_string(trackId) + " after");
    }

    std::vector<std::int64_t> expected = *before;
    std::erase(expected, trackId);
    expected.insert(afterTrackId ? std::find(expected.begin(), expected.end(), *afterTrackId) + 1 : expected.begin(),
                    trackId);
    if (expected == *before) {
        return false;
    }

    // Unlinked as trigger_before_delete_PlaylistEntity unlinks a deleted
    // entry: the one pointing at it takes its next.
    const std::int64_t entry = *subject;
    db.run("UPDATE PlaylistEntity SET nextEntityId = ? WHERE listId = ? AND nextEntityId = ? AND id <> ?;",
           {entries.at(entry).next, listId, entry, entry});
    // Linked in again as playlist_entity_table::add links a new one: it
    // takes the anchor's next (the head, to go first), and the entry that
    // pointed there (the anchor; none for the head) points at it.
    std::int64_t next = 0;
    const auto now = listEntries(db, listId);
    if (afterTrackId) {
        for (const auto &[id, e] : now) {
            if (e.trackId == *afterTrackId) {
                next = e.next;
                db.run("UPDATE PlaylistEntity SET nextEntityId = ? WHERE id = ?;", {entry, id});
                break;
            }
        }
    } else {
        // The head of the list without the entry: the one nothing but the
        // entry itself points at.
        std::set<std::int64_t> pointedAt;
        for (const auto &[id, e] : now) {
            if (id != entry) {
                pointedAt.insert(e.next);
            }
        }
        for (const auto &[id, e] : now) {
            if (id != entry && !pointedAt.count(id)) {
                next = id;
                break;
            }
        }
    }
    db.run("UPDATE PlaylistEntity SET nextEntityId = ? WHERE id = ?;", {next, entry});

    if (listInOrder(listEntries(db, listId)) != expected) {
        throw std::runtime_error("engine: \"" + playlistPath + "\" does not read back in the order asked after moving track id="
                                 + std::to_string(trackId));
    }
    db.commit();
    return true;
}

int engineSmartlistsFiledUnder(const std::string &engineLibraryPath, const std::string &playlistPath)
{
    std::string spelled;
    for (const auto &title : splitPlaylistPath(playlistPath)) {
        spelled += title + ";";
    }
    PlaylistWriter db(engineLibraryPath);
    bool hasTable = false;
    db.run("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'Smartlist';", {},
           [&](sqlite3_stmt *) { hasTable = true; });
    if (!hasTable) {
        return 0;
    }
    int filed = 0;
    // instr() rather than substr(): it counts characters, not the bytes
    // a UTF-8 title takes.
    db.run("SELECT count(*) FROM Smartlist WHERE instr(parentPlaylistPath, ?) = 1 OR instr(nextPlaylistPath, ?) = 1;",
           {spelled, spelled},
           [&](sqlite3_stmt *s) { filed = sqlite3_column_int(s, 0); });
    return filed;
}

int reassignEnginePlaylistEntries(const std::string &engineLibraryPath, std::int64_t doomedTrackId,
                                  std::int64_t survivorTrackId)
{
    PlaylistWriter db(engineLibraryPath);
    const std::string survivorUuid = entityUuidFor(db, survivorTrackId);
    std::set<std::int64_t> lists;
    db.run("SELECT DISTINCT listId FROM PlaylistEntity WHERE trackId = ?;", {doomedTrackId},
           [&](sqlite3_stmt *s) { lists.insert(sqlite3_column_int64(s, 0)); });
    int changed = 0;
    for (const std::int64_t listId : lists) {
        const auto entries = listEntries(db, listId);
        const auto before = listInOrder(entries);
        if (!before) {
            throw std::runtime_error("engine: the entries of list id=" + std::to_string(listId)
                                     + " do not form one chain; refusing to repoint them");
        }
        std::vector<std::int64_t> expected;
        bool survivorThere = std::find(before->begin(), before->end(), survivorTrackId) != before->end();
        for (const std::int64_t track : *before) {
            if (track == doomedTrackId) {
                if (!survivorThere) {
                    expected.push_back(survivorTrackId);
                    survivorThere = true;
                }
            } else {
                expected.push_back(track);
            }
        }
        // The first of the doomed track's entries takes the survivor
        // where the list has none; the rest go.
        std::map<std::int64_t, std::int64_t> nextOf;
        for (const auto &[id, e] : entries) {
            nextOf[id] = e.next;
        }
        // Named: a range-for over *walkChain(...) would read the optional
        // after it is gone (GCC 13 has not got C++23's P2718).
        const std::vector<std::int64_t> chain = *walkChain(nextOf);
        bool repointed = std::find(before->begin(), before->end(), survivorTrackId) != before->end();
        for (const std::int64_t id : chain) {
            if (entries.at(id).trackId != doomedTrackId) {
                continue;
            }
            if (!repointed) {
                db.run("UPDATE PlaylistEntity SET trackId = ?, databaseUuid = ? WHERE id = ?;",
                       {survivorTrackId, survivorUuid, id});
                repointed = true;
            } else {
                db.run("DELETE FROM PlaylistEntity WHERE id = ?;", {id});
            }
            ++changed;
        }
        if (listInOrder(listEntries(db, listId)) != expected) {
            throw std::runtime_error("engine: list id=" + std::to_string(listId)
                                     + " does not read back in its order after repointing track id="
                                     + std::to_string(doomedTrackId));
        }
    }
    db.commit();
    return changed;
}

namespace
{

// The entries the player cannot resolve (unresolvedPlaylistEntries), with
// the list they are in and the title of that list.
struct UnresolvedEntry
{
    std::int64_t id = 0;
    std::int64_t listId = 0;
    std::int64_t trackId = 0;
    std::string title;
};
// An entry resolves through a track whose (originDatabaseUuid,
// originTrackId) is its (databaseUuid, trackId), or, for a track naming
// no origin, through (the library's uuid, the track's id).
constexpr const char *UnresolvedEntriesSql =
    "SELECT e.id, e.listId, e.trackId, p.title FROM PlaylistEntity e "
    "JOIN Playlist p ON p.id = e.listId JOIN Track t ON t.id = e.trackId "
    "WHERE e.trackId > 0 "
    "AND NOT EXISTS (SELECT 1 FROM Track o WHERE o.originDatabaseUuid = e.databaseUuid AND o.originTrackId = e.trackId) "
    "AND NOT (e.databaseUuid = (SELECT uuid FROM Information LIMIT 1) "
    "         AND (t.originDatabaseUuid IS NULL OR t.originDatabaseUuid = '')) "
    "ORDER BY e.listId, e.id;";

std::vector<UnresolvedEntry> unresolvedEntries(PlaylistWriter &db)
{
    std::vector<UnresolvedEntry> found;
    db.run(UnresolvedEntriesSql, {}, [&](sqlite3_stmt *s) {
        const auto *title = reinterpret_cast<const char *>(sqlite3_column_text(s, 3));
        found.push_back({sqlite3_column_int64(s, 0), sqlite3_column_int64(s, 1), sqlite3_column_int64(s, 2),
                         title ? title : ""});
    });
    return found;
}

}  // namespace

std::vector<UnresolvedPlaylistEntries> unresolvedPlaylistEntries(const std::string &engineLibraryPath)
{
    std::vector<UnresolvedPlaylistEntries> perList;
    const std::string file = pathToUtf8(pathFromUtf8(engineLibraryPath) / "Database2" / "m.db");
    if (!std::filesystem::is_regular_file(pathFromUtf8(file))) {
        return perList;  // no Engine 2.x or 3.x library: none to resolve
    }
    // Read only, unlike the repair: a browsed backup is a read-only file.
    sqlite3 *handle = nullptr;
    if (sqlite3_open_v2(file.c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        const std::string why = handle ? sqlite3_errmsg(handle) : "out of memory";
        sqlite3_close(handle);
        throw std::runtime_error("engine: could not open " + file + ": " + why);
    }
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(handle, UnresolvedEntriesSql, -1, &stmt, nullptr) != SQLITE_OK) {
        const std::string why = sqlite3_errmsg(handle);
        sqlite3_close(handle);
        throw std::runtime_error("engine: could not look for playlist entries the player cannot see: " + why);
    }
    std::int64_t lastList = -1;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        const std::int64_t listId = sqlite3_column_int64(stmt, 1);
        if (listId != lastList) {
            const auto *title = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
            perList.push_back({title ? title : "", 0});
            lastList = listId;
        }
        ++perList.back().entries;
    }
    const std::string why = sqlite3_errmsg(handle);
    sqlite3_finalize(stmt);
    sqlite3_close(handle);
    if (step != SQLITE_DONE) {
        throw std::runtime_error("engine: stopped looking for playlist entries the player cannot see: " + why);
    }
    return perList;
}

RepointedPlaylistEntries repointUnresolvedPlaylistEntries(const std::string &engineLibraryPath)
{
    PlaylistWriter db(engineLibraryPath);
    RepointedPlaylistEntries done;
    const auto found = unresolvedEntries(db);
    // Each list touched as it was: its entries, and their ids in chain order.
    std::map<std::int64_t, std::pair<std::map<std::int64_t, ListEntry>, std::vector<std::int64_t>>> lists;
    for (const auto &e : found) {
        if (lists.count(e.listId)) {
            continue;
        }
        auto entries = listEntries(db, e.listId);
        std::map<std::int64_t, std::int64_t> nextOf;
        for (const auto &[id, entry] : entries) {
            nextOf[id] = entry.next;
        }
        const auto chain = walkChain(nextOf);
        if (!chain) {
            throw std::runtime_error("engine: the entries of \"" + e.title
                                     + "\" do not form one chain; refusing to repoint them");
        }
        lists[e.listId] = {std::move(entries), *chain};
    }
    std::set<std::int64_t> deleted;
    for (const auto &e : found) {
        std::int64_t originTrackId = e.trackId;
        db.run("SELECT originTrackId FROM Track WHERE id = ?;", {e.trackId}, [&](sqlite3_stmt *s) {
            if (sqlite3_column_type(s, 0) != SQLITE_NULL) {
                originTrackId = sqlite3_column_int64(s, 0);
            }
        });
        if (originTrackId != e.trackId) {
            // Repointing the uuid would not make it resolve: which track
            // the player would find instead is not ours to guess.
            ++done.leftAlone;
            continue;
        }
        const std::string uuid = entityUuidFor(db, e.trackId);
        bool twin = false;
        db.run("SELECT 1 FROM PlaylistEntity WHERE listId = ? AND trackId = ? AND databaseUuid = ? AND id <> ?;",
               {e.listId, e.trackId, uuid, e.id}, [&](sqlite3_stmt *) { twin = true; });
        if (twin) {
            // The list holds the track already, visibly: this copy of it
            // never showed. Engine's trigger relinks round it.
            db.run("DELETE FROM PlaylistEntity WHERE id = ?;", {e.id});
            deleted.insert(e.id);
            ++done.removed;
        } else {
            db.run("UPDATE PlaylistEntity SET databaseUuid = ? WHERE id = ?;", {uuid, e.id});
            ++done.repointed;
        }
    }
    for (const auto &[listId, list] : lists) {
        std::vector<std::int64_t> expected;
        for (const std::int64_t id : list.second) {
            if (!deleted.count(id)) {
                expected.push_back(list.first.at(id).trackId);
            }
        }
        if (listInOrder(listEntries(db, listId)) != expected) {
            throw std::runtime_error("engine: list id=" + std::to_string(listId)
                                     + " does not read back in its order after repointing its entries");
        }
    }
    db.commit();
    return done;
}

}  // namespace seabass::infrastructure::engine
