// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// removeEngineTrackRows takes a track out of an Engine m.db with every row
// that names it, and leaves the playlists it was in walkable.
//
// Over a copy of the committed anonymized fixture's Engine library (schema
// 3.0), with the track that is in the most playlists and one in none:
//
// - before, the listed track has its PerformanceData row and is in N >= 2
//   playlists;
// - after, no Track, PerformanceData, PlaylistEntity or PreparelistEntity
//   row names either, engineRowsNamingTrack says 0, the dangling entries
//   are the ones the fixture already had (none added), every playlist's
//   nextEntityId chain still walks from one head through every remaining
//   entry in the old order, and the other tracks are all still there;
// - an unknown id, a duplicate id and a missing error out-parameter are
//   refused, and a refused call removes nothing (the transaction rolls
//   back, the good id in the same call stays);
// - removeEngineTracks keeps refusing a track that is in a playlist, and
//   removes an unlisted one with its PerformanceData row.
//
// And over a fresh 2.18 database, where ChangeLog is a table naming
// tracks and PerformanceData a view: the ChangeLog rows stay with their
// trackId set to NULL, as the schema's ON DELETE SET NULL asks.

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <djinterop/djinterop.hpp>
#include <sqlite3.h>

#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/engine/engine_track_rows.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using namespace seabass::infrastructure::engine;

namespace
{

// A read-only look at an m.db, one query at a time.
std::vector<std::vector<std::int64_t>> rows(const std::string &file, const std::string &sql)
{
    sqlite3 *handle = nullptr;
    const int opened = sqlite3_open_v2(file.c_str(), &handle, SQLITE_OPEN_READONLY, nullptr);
    assert(opened == SQLITE_OK);
    sqlite3_stmt *stmt = nullptr;
    const int prepared = sqlite3_prepare_v2(handle, sql.c_str(), -1, &stmt, nullptr);
    if (prepared != SQLITE_OK) {
        std::cerr << sql << ": " << sqlite3_errmsg(handle) << "\n";
    }
    assert(prepared == SQLITE_OK);
    std::vector<std::vector<std::int64_t>> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::vector<std::int64_t> row;
        for (int c = 0; c < sqlite3_column_count(stmt); ++c) {
            row.push_back(sqlite3_column_type(stmt, c) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, c));
        }
        out.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);
    return out;
}

std::int64_t scalar(const std::string &file, const std::string &sql)
{
    const auto r = rows(file, sql);
    assert(r.size() == 1 && r[0].size() == 1);
    return r[0][0];
}

std::int64_t countWhere(const std::string &file, const std::string &table, const std::string &column, std::int64_t id)
{
    return scalar(file, "SELECT count(*) FROM " + table + " WHERE " + column + " = " + std::to_string(id) + ";");
}

// Each playlist's track ids in chain order: from the one entry no other
// entry of the list points at, along nextEntityId to 0. Asserts there is
// exactly one head, no entry is visited twice, and the walk reaches every
// entry the list has.
std::map<std::int64_t, std::vector<std::int64_t>> chains(const std::string &file)
{
    struct Entry
    {
        std::int64_t trackId;
        std::int64_t next;
    };
    std::map<std::int64_t, std::map<std::int64_t, Entry>> lists;
    for (const auto &r : rows(file, "SELECT listId, id, trackId, nextEntityId FROM PlaylistEntity;")) {
        lists[r[0]][r[1]] = Entry{r[2], r[3]};
    }
    std::map<std::int64_t, std::vector<std::int64_t>> out;
    for (const auto &[listId, entries] : lists) {
        std::set<std::int64_t> pointedAt;
        for (const auto &[id, e] : entries) {
            pointedAt.insert(e.next);
        }
        std::vector<std::int64_t> heads;
        for (const auto &[id, e] : entries) {
            if (!pointedAt.count(id)) {
                heads.push_back(id);
            }
        }
        if (heads.size() != 1) {
            std::cerr << "playlist " << listId << " has " << heads.size() << " heads\n";
        }
        assert(heads.size() == 1);
        std::set<std::int64_t> visited;
        std::vector<std::int64_t> order;
        for (std::int64_t at = heads[0]; at != 0;) {
            const auto it = entries.find(at);
            assert(it != entries.end() && "the chain points at an entry of the same list");
            assert(visited.insert(at).second && "the chain does not loop");
            order.push_back(it->second.trackId);
            at = it->second.next;
        }
        assert(order.size() == entries.size() && "the chain reaches every entry");
        out[listId] = order;
    }
    return out;
}

bool same(const std::vector<DanglingPlaylistEntries> &a, const std::vector<DanglingPlaylistEntries> &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].playlist != b[i].playlist || a[i].entries != b[i].entries) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: engine_track_rows_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    const fs::path fixture = pathFromUtf8(argv[1]);
    const fs::path scratch = testing::scratchRoot() / "engine_track_rows";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    // A copy, never the fixture itself: opening its m.db in place could
    // roll a journal into it or delete committed -wal/-shm files.
    const fs::path library = scratch / "Engine Library";
    fs::copy(fixture / "engine", library, fs::copy_options::recursive);
    const std::string libraryUtf8 = pathToUtf8(library);
    const std::string db = pathToUtf8(library / "Database2" / "m.db");

    // The track in the most playlists, and one in none; both with a
    // PerformanceData row.
    const auto busiest = rows(db, "SELECT e.trackId, count(*) c FROM PlaylistEntity e JOIN Track t ON t.id = e.trackId "
                                  "JOIN PerformanceData p ON p.trackId = t.id GROUP BY e.trackId ORDER BY c DESC, e.trackId "
                                  "LIMIT 1;");
    assert(busiest.size() == 1);
    const std::int64_t listed = busiest[0][0];
    const std::int64_t playlists = busiest[0][1];
    const std::int64_t unlisted =
        scalar(db, "SELECT min(t.id) FROM Track t JOIN PerformanceData p ON p.trackId = t.id "
                   "WHERE t.id NOT IN (SELECT trackId FROM PlaylistEntity);");
    assert(unlisted > 0);
    std::cout << "listed track id=" << listed << " in " << playlists << " playlists, unlisted id=" << unlisted << "\n";

    // 1. Before.
    assert(playlists >= 2 && "the chosen track is in several playlists");
    assert(countWhere(db, "PerformanceData", "trackId", listed) == 1 && "it has its PerformanceData row");
    assert(countWhere(db, "PerformanceData", "trackId", unlisted) == 1);
    std::string error;
    assert(engineRowsNamingTrack(db, listed, &error) == 1 + 1 + playlists && error.empty());
    const std::int64_t tracksBefore = scalar(db, "SELECT count(*) FROM Track;");
    const std::int64_t performanceBefore = scalar(db, "SELECT count(*) FROM PerformanceData;");
    const auto danglingBefore = danglingPlaylistEntries(libraryUtf8);
    const auto chainsBefore = chains(db);
    std::cout << "case 1 (before: PerformanceData row, " << playlists << " playlists, chains walk) OK\n";

    // 2. Refusals remove nothing: an unknown id after a good one, a
    //    duplicate, and no error out-parameter.
    const std::int64_t unknown = scalar(db, "SELECT max(id) FROM Track;") + 1000;
    error.clear();
    assert(removeEngineTrackRows(db, {unlisted, unknown}, &error) == -1);
    assert(error.find("no track id=" + std::to_string(unknown)) != std::string::npos);
    assert(countWhere(db, "Track", "id", unlisted) == 1 && "rolled back: the good id in the same call stays");
    assert(countWhere(db, "PerformanceData", "trackId", unlisted) == 1 && "and keeps its PerformanceData");
    error.clear();
    assert(removeEngineTrackRows(db, {unlisted, unlisted}, &error) == -1 && !error.empty());
    assert(countWhere(db, "Track", "id", unlisted) == 1);
    bool threw = false;
    try {
        (void)removeEngineTrackRows(db, {unlisted}, nullptr);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    assert(threw && "the error out-parameter is required");
    assert(countWhere(db, "Track", "id", unlisted) == 1);
    error = "stale";
    assert(removeEngineTrackRows(db, {}, &error) == 0 && error.empty());
    assert(scalar(db, "SELECT count(*) FROM Track;") == tracksBefore);
    std::cout << "case 2 (unknown, duplicate, no out-parameter: refused, nothing removed) OK\n";

    // 3. removeEngineTracks keeps Delete Tracks' rule: a listed track is
    //    refused, before anything is written.
    threw = false;
    try {
        (void)removeEngineTracks(libraryUtf8, {unlisted, listed});
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw && "removeEngineTracks refuses a track in a playlist");
    assert(countWhere(db, "Track", "id", listed) == 1 && countWhere(db, "Track", "id", unlisted) == 1
           && "and removes nothing, not even the unlisted one");
    std::cout << "case 3 (removeEngineTracks refuses a listed track, removes nothing) OK\n";

    // 4. Both go, with everything that names them.
    error = "stale";
    const int removed = removeEngineTrackRows(db, {listed, unlisted}, &error);
    if (removed != 2) {
        std::cerr << "removeEngineTrackRows: " << removed << ", " << error << "\n";
    }
    assert(removed == 2 && error.empty());
    for (const std::int64_t id : {listed, unlisted}) {
        assert(countWhere(db, "Track", "id", id) == 0);
        assert(countWhere(db, "PerformanceData", "trackId", id) == 0 && "no PerformanceData left behind");
        assert(countWhere(db, "PlaylistEntity", "trackId", id) == 0);
        assert(countWhere(db, "PreparelistEntity", "trackId", id) == 0);
        assert(engineRowsNamingTrack(db, id, &error) == 0 && error.empty());
    }
    assert(scalar(db, "SELECT count(*) FROM Track;") == tracksBefore - 2 && "every other track is still there");
    assert(scalar(db, "SELECT count(*) FROM PerformanceData;") == performanceBefore - 2);
    assert(same(danglingPlaylistEntries(libraryUtf8), danglingBefore) && "no playlist entry left dangling");
    const auto chainsAfter = chains(db);
    std::int64_t affected = 0;
    for (const auto &[listId, before] : chainsBefore) {
        std::vector<std::int64_t> expected;
        for (const std::int64_t t : before) {
            if (t != listed && t != unlisted) {
                expected.push_back(t);
            }
        }
        affected += expected.size() != before.size() ? 1 : 0;
        const auto it = chainsAfter.find(listId);
        if (expected.empty()) {
            assert(it == chainsAfter.end());
            continue;
        }
        assert(it != chainsAfter.end() && it->second == expected && "the rest of the list, in its old order");
    }
    assert(affected == playlists && "it left exactly the playlists it was in");
    // And as libdjinterop reads them, which is how the Engine reader does:
    // it walks each chain backwards from its tail, so a broken link would
    // cut the list short. Every entry is still reached (the fixture's
    // dangling ones included; libdjinterop hands those out too).
    {
        auto engine = djinterop::engine::load_database(libraryUtf8);
        std::int64_t members = 0;
        std::vector<djinterop::playlist> pending = engine.root_playlists();
        while (!pending.empty()) {
            const auto pl = pending.back();
            pending.pop_back();
            for (const auto &t : pl.tracks()) {
                assert(t.id() != listed);
                ++members;
            }
            for (const auto &child : pl.children()) {
                pending.push_back(child);
            }
        }
        assert(members == scalar(db, "SELECT count(*) FROM PlaylistEntity;"));
    }
    std::cout << "case 4 (removed with every row naming it; " << affected << " playlists still walk) OK\n";

    // 5. A second call on the same ids: unknown now, an error.
    error.clear();
    assert(removeEngineTrackRows(db, {listed}, &error) == -1 && !error.empty());
    std::cout << "case 5 (removing a removed track is an error) OK\n";

    // 6. removeEngineTracks on an unlisted track takes its PerformanceData
    //    row along (it used to be left behind).
    {
        const std::int64_t another =
            scalar(db, "SELECT min(t.id) FROM Track t JOIN PerformanceData p ON p.trackId = t.id "
                       "WHERE t.id NOT IN (SELECT trackId FROM PlaylistEntity);");
        assert(another > 0);
        assert(removeEngineTracks(libraryUtf8, {another}) == 1);
        assert(countWhere(db, "Track", "id", another) == 0);
        assert(countWhere(db, "PerformanceData", "trackId", another) == 0 && "removeEngineTracks leaves no PerformanceData");
    }
    std::cout << "case 6 (removeEngineTracks takes PerformanceData along) OK\n";

    // 7. Schema 2.18: ChangeLog is a table naming tracks, PerformanceData a
    //    view over Track.
    {
        const fs::path old = scratch / "Engine 2.18";
        std::map<char, std::int64_t> id;
        {
            auto engine = djinterop::engine::create_database(pathToUtf8(old), djinterop::engine::engine_schema::schema_2_18_0);
            auto list = engine.create_root_playlist("L");
            for (const char *name : {"a.mp3", "b.mp3", "c.mp3"}) {
                djinterop::track_snapshot s;
                s.relative_path = std::string("../Music/") + name;
                auto t = engine.create_track(s);
                list.add_track_back(t);
                id[name[0]] = t.id();
            }
            auto b = engine.track_by_id(id['b']);
            b->set_title(std::string("B"));
        }
        const std::int64_t gone = id['b'];
        const std::string oldDb = pathToUtf8(old / "Database2" / "m.db");
        assert(scalar(oldDb, "SELECT count(*) FROM sqlite_master WHERE name = 'ChangeLog' AND type = 'table';") == 1);
        assert(scalar(oldDb, "SELECT count(*) FROM sqlite_master WHERE name = 'PerformanceData' AND type = 'view';") == 1);
        assert(countWhere(oldDb, "ChangeLog", "trackId", gone) > 0 && "the update put b in ChangeLog");
        const std::int64_t logBefore = scalar(oldDb, "SELECT count(*) FROM ChangeLog;");
        const auto oldChains = chains(oldDb);
        error = "stale";
        assert(removeEngineTrackRows(oldDb, {gone}, &error) == 1 && error.empty());
        assert(countWhere(oldDb, "Track", "id", gone) == 0);
        assert(countWhere(oldDb, "ChangeLog", "trackId", gone) == 0);
        assert(scalar(oldDb, "SELECT count(*) FROM ChangeLog;") == logBefore && "the log entries stay, trackId NULL");
        assert(engineRowsNamingTrack(oldDb, gone, &error) == 0 && error.empty());
        const auto oldAfter = chains(oldDb);
        assert(oldChains.size() == 1 && oldAfter.size() == 1);
        assert(oldAfter.begin()->second == (std::vector<std::int64_t>{id['a'], id['c']}) && "a and c, in order");
    }
    std::cout << "case 7 (schema 2.18: ChangeLog kept with trackId NULL) OK\n";

    std::cout << "engine_track_rows_test: all cases passed\n";
    return 0;
}
