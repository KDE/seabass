// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// createEnginePlaylist, renameEnginePlaylist, insertIntoEnginePlaylist
// and removeFromEnginePlaylist over a copy of the committed anonymized fixture's Engine library: 33
// root playlists "Playlist 000" (id 231, 100 entries) to "Playlist 032",
// no folders, 2074 entries, 8 of them dangling in "Playlist 002".
//
// - create makes a root playlist last among the roots, a missing folder
//   with a list inside it, and a list in an existing folder, each read
//   back by path exactly once; it refuses a path that exists, a prefix
//   two playlists spell, and a missing prefix that a "/" title already
//   spells further down;
// - m.db itself refuses a second playlist of one name under one parent,
//   so the ambiguity is planted the one way it happens: a root title
//   holding a "/";
// - rename in place keeps the id, the entries and their order, and the
//   playlist's place among the roots; rename into a folder moves a
//   playlist from the middle of the roots to the end of the folder with
//   both sibling chains whole; a target that exists, a parent that does
//   not, a move into itself and an ambiguous source are refused;
// - insert after a member lands right after it, the chain walking whole
//   forwards and backwards and libdjinterop's tracks() agreeing; an
//   anchor that is not a member and no anchor both append; a member is
//   left where it is; an unknown track and an ambiguous path are refused;
// - remove takes a mid-list member of "Playlist 000" out, whose entry
//   ids (11083 up) are nowhere near its track ids (1574 at most), the
//   other 99 walking whole both ways and libdjinterop's tracks()
//   agreeing; with an entry planted whose own id is another member's
//   track id, removing that member takes its own entry and leaves the
//   planted one (libdjinterop's playlist::remove_track deletes by entry
//   id and fails both); head and tail go too; an unknown track or list
//   is refused;
// - after every call the dangling entries are the fixture's 8, every
//   list's chain is whole, and every playlist and entry the call was not
//   about is as it was;
// - libdjinterop's own playlist::set_parent, on a second copy, leaves a
//   playlist moved out of the middle of its siblings out of the new
//   folder's children: pinned, because the header says why rename does
//   not use it.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <djinterop/djinterop.hpp>
#include <sqlite3.h>

#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using namespace seabass::infrastructure::engine;

namespace
{

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

// Runs one statement on a copy; returns whether SQLite took it.
bool exec(const std::string &file, const std::string &sql)
{
    sqlite3 *handle = nullptr;
    const int opened = sqlite3_open_v2(file.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr);
    assert(opened == SQLITE_OK);
    char *error = nullptr;
    const bool ok = sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, &error) == SQLITE_OK;
    if (!ok) {
        std::cout << "  sqlite refused: " << (error ? error : "?") << "\n";
    }
    sqlite3_free(error);
    sqlite3_close(handle);
    return ok;
}

// One linked list (id -> next id, 0 ending it) walked from the head
// forwards and from the tail backwards. Asserts one head, one tail, no
// loop, every row reached both ways and the two orders mirror each other.
std::vector<std::int64_t> walk(const std::map<std::int64_t, std::int64_t> &nextOf, const std::string &what)
{
    if (nextOf.empty()) {
        return {};
    }
    std::set<std::int64_t> pointedAt;
    std::map<std::int64_t, std::int64_t> previousOf;
    std::vector<std::int64_t> tails;
    for (const auto &[id, next] : nextOf) {
        pointedAt.insert(next);
        if (next == 0) {
            tails.push_back(id);
        } else {
            const bool once = previousOf.emplace(next, id).second;
            if (!once) {
                std::cerr << what << ": two rows point at " << next << "\n";
            }
            assert(once);
        }
    }
    std::vector<std::int64_t> heads;
    for (const auto &[id, next] : nextOf) {
        if (!pointedAt.count(id)) {
            heads.push_back(id);
        }
    }
    if (heads.size() != 1 || tails.size() != 1) {
        std::cerr << what << ": " << heads.size() << " heads, " << tails.size() << " tails\n";
    }
    assert(heads.size() == 1 && tails.size() == 1);
    std::vector<std::int64_t> forwards;
    std::set<std::int64_t> seen;
    for (std::int64_t at = heads[0]; at != 0; at = nextOf.at(at)) {
        assert(nextOf.count(at) && "the chain points inside its own list");
        assert(seen.insert(at).second && "the chain does not loop");
        forwards.push_back(at);
    }
    if (forwards.size() != nextOf.size()) {
        std::cerr << what << ": forwards reaches " << forwards.size() << " of " << nextOf.size() << "\n";
    }
    assert(forwards.size() == nextOf.size());
    std::vector<std::int64_t> backwards;
    for (std::int64_t at = tails[0];;) {
        backwards.insert(backwards.begin(), at);
        const auto it = previousOf.find(at);
        if (it == previousOf.end()) {
            break;
        }
        at = it->second;
        assert(backwards.size() <= nextOf.size());
    }
    assert(backwards == forwards && "backwards from the tail reaches the same rows in the same order");
    return forwards;
}

// Every list's track ids in chain order.
std::map<std::int64_t, std::vector<std::int64_t>> entryChains(const std::string &db)
{
    std::map<std::int64_t, std::map<std::int64_t, std::int64_t>> nextOf;
    std::map<std::int64_t, std::int64_t> trackOf;
    for (const auto &r : rows(db, "SELECT listId, id, trackId, nextEntityId FROM PlaylistEntity;")) {
        nextOf[r[0]][r[1]] = r[3];
        trackOf[r[1]] = r[2];
    }
    std::map<std::int64_t, std::vector<std::int64_t>> out;
    for (const auto &[listId, chain] : nextOf) {
        for (const std::int64_t entity : walk(chain, "list " + std::to_string(listId))) {
            out[listId].push_back(trackOf.at(entity));
        }
    }
    return out;
}

// Every parent's children in nextListId order (0: the roots).
std::map<std::int64_t, std::vector<std::int64_t>> playlistChains(const std::string &db)
{
    std::map<std::int64_t, std::map<std::int64_t, std::int64_t>> nextOf;
    for (const auto &r : rows(db, "SELECT parentListId, id, nextListId FROM Playlist;")) {
        nextOf[r[0]][r[1]] = r[2];
    }
    std::map<std::int64_t, std::vector<std::int64_t>> out;
    for (const auto &[parent, chain] : nextOf) {
        out[parent] = walk(chain, "children of " + std::to_string(parent));
    }
    return out;
}

std::int64_t playlistId(const std::string &db, const std::string &title, std::int64_t parent)
{
    const auto r = rows(db, "SELECT id FROM Playlist WHERE title = '" + title + "' AND parentListId = "
                                + std::to_string(parent) + ";");
    assert(r.size() == 1);
    return r[0][0];
}

std::vector<std::int64_t> djinteropTracks(const std::string &library, const std::vector<std::string> &path)
{
    auto db = djinterop::engine::load_database(library);
    auto pl = db.root_playlist_by_name(path.front());
    assert(pl);
    for (size_t i = 1; i < path.size(); ++i) {
        pl = pl->sub_playlist_by_name(path[i]);
        assert(pl);
    }
    std::vector<std::int64_t> ids;
    for (const auto &t : pl->tracks()) {
        ids.push_back(t.id());
    }
    return ids;
}

bool throws(const std::function<void()> &f, const std::string &label)
{
    try {
        f();
    } catch (const std::exception &e) {
        std::cout << "  refused (" << label << "): " << e.what() << "\n";
        return true;
    }
    std::cerr << label << ": not refused\n";
    return false;
}

std::vector<std::pair<std::string, int>> dangling(const std::string &library)
{
    std::vector<std::pair<std::string, int>> out;
    for (const auto &d : danglingPlaylistEntries(library)) {
        out.emplace_back(d.playlist, d.entries);
    }
    return out;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: engine_playlists_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    const fs::path fixture = pathFromUtf8(argv[1]);
    const fs::path scratch = testing::scratchRoot() / "engine_playlists";
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    // A copy, never the fixture itself.
    const fs::path libraryPath = scratch / "Engine Library";
    fs::copy(fixture / "engine", libraryPath, fs::copy_options::recursive);
    const std::string library = pathToUtf8(libraryPath);
    const std::string db = pathToUtf8(libraryPath / "Database2" / "m.db");

    // The fixture as this test reads it.
    assert(scalar(db, "SELECT count(*) FROM Playlist;") == 33);
    assert(scalar(db, "SELECT count(*) FROM Playlist WHERE parentListId <> 0;") == 0);
    assert(scalar(db, "SELECT count(*) FROM PlaylistEntity;") == 2074);
    assert(playlistId(db, "Playlist 000", 0) == 231);
    assert(scalar(db, "SELECT count(*) FROM PlaylistEntity WHERE listId = 231;") == 100);
    const auto fixtureDangling = dangling(library);
    assert(fixtureDangling.size() == 1 && fixtureDangling[0].first == "Playlist 002" && fixtureDangling[0].second == 8);

    auto entries = entryChains(db);
    auto playlists = playlistChains(db);
    assert(playlists.size() == 1 && playlists.at(0).size() == 33);
    assert(entries.at(231).size() == 100);

    // Whatever a step was not about stays as it was; everything walks.
    const auto unchangedApartFrom = [&](const std::set<std::int64_t> &touchedLists,
                                        const std::set<std::int64_t> &touchedParents) {
        const auto e = entryChains(db);
        const auto p = playlistChains(db);
        for (const auto &[listId, order] : entries) {
            if (!touchedLists.count(listId)) {
                assert(e.count(listId) && e.at(listId) == order);
            }
        }
        for (const auto &[listId, order] : e) {
            assert(touchedLists.count(listId) || entries.count(listId));
        }
        for (const auto &[parent, order] : playlists) {
            if (!touchedParents.count(parent)) {
                assert(p.count(parent) && p.at(parent) == order);
            }
        }
        assert(dangling(library) == fixtureDangling);
        entries = e;
        playlists = p;
    };

    // m.db keeps titles unique within a parent: two playlists can spell one
    // path only through a "/" in a title.
    assert(!exec(db, "INSERT INTO Playlist (title, parentListId, isPersisted, nextListId, lastEditTime, "
                     "isExplicitlyExported) VALUES ('Playlist 001', 0, 1, 0, 0, 1);"));
    unchangedApartFrom({}, {});

    // --- create ---------------------------------------------------------
    std::cout << "create\n";
    const std::int64_t top = createEnginePlaylist(library, "Sync Top");
    assert(top == playlistId(db, "Sync Top", 0));
    assert(enginePlaylistExists(library, "Sync Top") && enginePlaylistCountAtPath(library, "Sync Top") == 1);
    assert(playlistChains(db).at(0).back() == top);
    assert(scalar(db, "SELECT count(*) FROM PlaylistEntity WHERE listId = " + std::to_string(top) + ";") == 0);
    unchangedApartFrom({}, {0});
    assert(playlists.at(0).size() == 34);

    const std::int64_t newList = createEnginePlaylist(library, "New Folder/New List");
    const std::int64_t folder = playlistId(db, "New Folder", 0);
    assert(newList == playlistId(db, "New List", folder));
    assert(enginePlaylistExists(library, "New Folder") && enginePlaylistExists(library, "New Folder/New List"));
    assert(enginePlaylistCountAtPath(library, "New Folder") == 1);
    assert(enginePlaylistCountAtPath(library, "New Folder/New List") == 1);
    unchangedApartFrom({}, {0, folder});
    assert(playlists.at(0).size() == 35 && playlists.at(0).back() == folder);
    assert(playlists.at(folder) == std::vector<std::int64_t>{newList});

    const std::int64_t second = createEnginePlaylist(library, "New Folder/Second List");
    assert(second == playlistId(db, "Second List", folder));
    assert(enginePlaylistCountAtPath(library, "New Folder/Second List") == 1);
    unchangedApartFrom({}, {folder});
    assert((playlists.at(folder) == std::vector<std::int64_t>{newList, second}));

    // A playlist with entries holds a child the same way (a folder in
    // Engine is any playlist with children).
    const std::int64_t inside = createEnginePlaylist(library, "Playlist 031/Inside");
    assert(inside == playlistId(db, "Inside", playlistId(db, "Playlist 031", 0)));
    unchangedApartFrom({}, {playlistId(db, "Playlist 031", 0)});

    assert(throws([&] { createEnginePlaylist(library, "Playlist 001"); }, "exists"));
    assert(throws([&] { createEnginePlaylist(library, "New Folder/New List"); }, "exists in a folder"));
    assert(throws([&] { createEnginePlaylist(library, ""); }, "empty path"));
    assert(throws([&] { createEnginePlaylist(library, "A//B"); }, "empty title"));
    assert(throws([&] { createEnginePlaylist(library, "Semi;colon"); }, "a ';'"));
    unchangedApartFrom({}, {});

    // Plant the ambiguity: a root playlist titled "New Folder/New List".
    {
        auto d = djinterop::engine::load_database(library);
        d.create_root_playlist("New Folder/New List");
        d.create_root_playlist("Ghost/Child");
    }
    const std::int64_t plantedAmbiguous = playlistId(db, "New Folder/New List", 0);
    const std::int64_t plantedGhost = playlistId(db, "Ghost/Child", 0);
    assert(enginePlaylistCountAtPath(library, "New Folder/New List") == 2);
    unchangedApartFrom({}, {0});
    assert(throws([&] { createEnginePlaylist(library, "New Folder/New List/Deeper"); }, "ambiguous prefix"));
    // "Ghost" is missing, but "Ghost/Child" already reads as one playlist:
    // creating Ghost and Child would make it two.
    assert(throws([&] { createEnginePlaylist(library, "Ghost/Child/Leaf"); }, "missing prefix spelled below"));
    unchangedApartFrom({}, {});

    // --- rename ---------------------------------------------------------
    std::cout << "rename\n";
    const auto list231 = entries.at(231);
    const auto roots = playlists.at(0);
    assert(!renameEnginePlaylist(library, "Playlist 000", "Playlist 000"));
    assert(renameEnginePlaylist(library, "Playlist 000", "Playlist 000 renamed"));
    assert(playlistId(db, "Playlist 000 renamed", 0) == 231);
    assert(!enginePlaylistExists(library, "Playlist 000"));
    assert(enginePlaylistCountAtPath(library, "Playlist 000 renamed") == 1);
    unchangedApartFrom({}, {});
    assert(entries.at(231) == list231);
    assert(playlists.at(0) == roots);
    assert(djinteropTracks(library, {"Playlist 000 renamed"}) == list231);

    // "Playlist 005" sits in the middle of the roots: the case set_parent
    // gets wrong.
    const std::int64_t moved = playlistId(db, "Playlist 005", 0);
    assert(moved == 250 && roots.front() != moved && roots.back() != moved);
    const auto list250 = entries.at(moved);
    assert(list250.size() == 129);
    assert(renameEnginePlaylist(library, "Playlist 005", "New Folder/Moved 005"));
    assert(playlistId(db, "Moved 005", folder) == moved);
    assert(!enginePlaylistExists(library, "Playlist 005"));
    assert(enginePlaylistCountAtPath(library, "New Folder/Moved 005") == 1);
    unchangedApartFrom({}, {0, folder});
    assert(entries.at(moved) == list250);
    std::vector<std::int64_t> rootsWithout = roots;
    rootsWithout.erase(std::find(rootsWithout.begin(), rootsWithout.end(), moved));
    assert(playlists.at(0) == rootsWithout);
    assert((playlists.at(folder) == std::vector<std::int64_t>{newList, second, moved}));
    assert(djinteropTracks(library, {"New Folder", "Moved 005"}) == list250);
    {
        auto d = djinterop::engine::load_database(library);
        assert(d.root_playlist_by_name("New Folder")->children().size() == 3);
        assert(d.root_playlists().size() == rootsWithout.size());
    }

    assert(throws([&] { renameEnginePlaylist(library, "Playlist 001", "Playlist 002"); }, "target exists"));
    assert(throws([&] { renameEnginePlaylist(library, "Playlist 001", "No Such/Playlist 001"); }, "no new parent"));
    assert(throws([&] { renameEnginePlaylist(library, "New Folder", "New Folder/Second List/Folder"); }, "into itself"));
    assert(throws([&] { renameEnginePlaylist(library, "Playlist 404", "Playlist 405"); }, "no source"));
    assert(throws([&] { renameEnginePlaylist(library, "New Folder/New List", "Elsewhere"); }, "ambiguous source"));
    assert(throws([&] { renameEnginePlaylist(library, "Playlist 001", "Ghost/Child"); }, "target spelled by a title"));
    // Renaming "New Folder" to "Ghost" would respell its "Child"-less
    // children only, so make one that collides: "Ghost/Child" is taken.
    createEnginePlaylist(library, "New Folder/Child");
    const std::int64_t child = playlistId(db, "Child", folder);
    unchangedApartFrom({}, {folder});
    assert(throws([&] { renameEnginePlaylist(library, "New Folder", "Ghost"); }, "a child would become ambiguous"));
    unchangedApartFrom({}, {});
    (void)child;
    (void)plantedAmbiguous;
    (void)plantedGhost;

    // --- insert ---------------------------------------------------------
    std::cout << "insert\n";
    const std::string renamed = "Playlist 000 renamed";
    const auto before = entries.at(231);
    std::set<std::int64_t> members(before.begin(), before.end());
    std::vector<std::int64_t> outsiders;
    for (const auto &r : rows(db, "SELECT id FROM Track ORDER BY id;")) {
        if (!members.count(r[0])) {
            outsiders.push_back(r[0]);
        }
    }
    assert(outsiders.size() >= 4);
    const std::int64_t anchor = before.at(9);

    assert(insertIntoEnginePlaylist(library, renamed, outsiders[0], anchor));
    std::vector<std::int64_t> expected = before;
    expected.insert(expected.begin() + 10, outsiders[0]);
    unchangedApartFrom({231}, {});
    assert(entries.at(231) == expected);
    assert(entries.at(231).at(9) == anchor && entries.at(231).at(10) == outsiders[0]);
    assert(djinteropTracks(library, {renamed}) == expected);

    // An anchor that is not a member: appended.
    assert(!members.count(outsiders[3]));
    assert(insertIntoEnginePlaylist(library, renamed, outsiders[1], outsiders[3]));
    expected.push_back(outsiders[1]);
    unchangedApartFrom({231}, {});
    assert(entries.at(231) == expected);
    assert(djinteropTracks(library, {renamed}) == expected);

    // No anchor: appended.
    assert(insertIntoEnginePlaylist(library, renamed, outsiders[2], std::nullopt));
    expected.push_back(outsiders[2]);
    unchangedApartFrom({231}, {});
    assert(entries.at(231) == expected);
    assert(djinteropTracks(library, {renamed}) == expected);
    assert(expected.size() == 103);

    // A member stays where it is, whatever the anchor.
    assert(!insertIntoEnginePlaylist(library, renamed, outsiders[0], before.at(50)));
    assert(!insertIntoEnginePlaylist(library, renamed, before.at(0), std::nullopt));
    unchangedApartFrom({}, {});

    // An empty list; an anchor at the tail; an anchor with an entry
    // after it.
    assert(insertIntoEnginePlaylist(library, "New Folder/Second List", outsiders[0], std::nullopt));
    assert(insertIntoEnginePlaylist(library, "New Folder/Second List", outsiders[1], outsiders[0]));
    assert(insertIntoEnginePlaylist(library, "New Folder/Second List", outsiders[2], outsiders[0]));
    unchangedApartFrom({second}, {});
    assert((entries.at(second) == std::vector<std::int64_t>{outsiders[0], outsiders[2], outsiders[1]}));
    assert(djinteropTracks(library, {"New Folder", "Second List"}) == entries.at(second));

    assert(throws([&] { insertIntoEnginePlaylist(library, renamed, 999999, std::nullopt); }, "no track"));
    assert(throws([&] { insertIntoEnginePlaylist(library, "New Folder/New List", outsiders[0], std::nullopt); },
                  "ambiguous list"));
    assert(throws([&] { insertIntoEnginePlaylist(library, "Playlist 404", outsiders[0], std::nullopt); }, "no list"));
    unchangedApartFrom({}, {});

    // A list whose chain is already broken is refused: unlink the head of
    // "Playlist 001" from the rest by hand.
    const std::int64_t list246 = playlistId(db, "Playlist 001", 0);
    const auto broken = rows(db, "SELECT id, nextEntityId FROM PlaylistEntity WHERE listId = " + std::to_string(list246)
                                     + " AND id NOT IN (SELECT nextEntityId FROM PlaylistEntity WHERE listId = "
                                     + std::to_string(list246) + ");");
    assert(broken.size() == 1);
    assert(exec(db, "UPDATE PlaylistEntity SET nextEntityId = 0 WHERE id = " + std::to_string(broken[0][0]) + ";"));
    assert(throws([&] { insertIntoEnginePlaylist(library, "Playlist 001", outsiders[0], std::nullopt); },
                  "broken chain"));
    assert(exec(db, "UPDATE PlaylistEntity SET nextEntityId = " + std::to_string(broken[0][1])
                        + " WHERE id = " + std::to_string(broken[0][0]) + ";"));
    unchangedApartFrom({}, {});

    // Every row written carries what libdjinterop's own rows carry: a
    // "%F %T" UTC time to the nanosecond, persisted, explicitly exported.
    assert(scalar(db, "SELECT count(*) FROM Playlist WHERE lastEditTime GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9] "
                      "[0-9][0-9]:[0-9][0-9]:[0-9][0-9].[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]' "
                      "AND isPersisted = 1 AND isExplicitlyExported = 1;")
           == scalar(db, "SELECT count(*) FROM Playlist;"));
    assert(scalar(db, "SELECT count(DISTINCT databaseUuid) FROM PlaylistEntity;") == 1);
    assert(scalar(db, "SELECT count(*) FROM PlaylistEntity WHERE membershipReference <> 0;") == 0);

    // --- libdjinterop's set_parent, pinned ------------------------------
    {
        const fs::path otherPath = scratch / "set_parent" / "Engine Library";
        fs::create_directories(otherPath.parent_path());
        fs::copy(fixture / "engine", otherPath, fs::copy_options::recursive);
        auto d = djinterop::engine::load_database(pathToUtf8(otherPath));
        auto target = *d.root_playlist_by_name("Playlist 008");
        // A child already there, so the new folder's chain has a tail to
        // walk from (an empty one would trip libdjinterop's assert).
        target.create_sub_playlist("Already There");
        auto subject = *d.root_playlist_by_name("Playlist 010");
        subject.set_parent(target);
        const std::string otherDb = pathToUtf8(otherPath / "Database2" / "m.db");
        const auto children = d.root_playlist_by_name("Playlist 008")->children();
        const std::int64_t rowsUnder = scalar(otherDb, "SELECT count(*) FROM Playlist WHERE parentListId = 253;");
        std::cout << "set_parent: Playlist 008 holds " << rowsUnder << " rows, children() lists " << children.size()
                  << "\n";
        assert(rowsUnder == 2);
        assert(children.size() == 1 && children[0].name() == "Already There"
               && "set_parent left the moved playlist out of its new parent's children");
    }

    // --- remove ---------------------------------------------------------
    // On a fresh copy: the fixture's entry ids (11083 up) are far from its
    // track ids (1574 at most), the arrangement libdjinterop's
    // playlist::remove_track gets wrong (it deletes by the entry's id).
    std::cout << "remove\n";
    {
        const fs::path removePath = scratch / "remove" / "Engine Library";
        fs::create_directories(removePath.parent_path());
        fs::copy(fixture / "engine", removePath, fs::copy_options::recursive);
        const std::string lib = pathToUtf8(removePath);
        const std::string rdb = pathToUtf8(removePath / "Database2" / "m.db");
        const auto entriesBefore = entryChains(rdb);
        const auto playlistsBefore = playlistChains(rdb);
        const auto danglingBefore = dangling(lib);
        assert(danglingBefore == fixtureDangling);
        const std::vector<std::int64_t> members = entriesBefore.at(231);
        assert(members.size() == 100);
        assert(scalar(rdb, "SELECT min(id) FROM PlaylistEntity WHERE listId = 231;")
               > scalar(rdb, "SELECT max(id) FROM Track;"));

        // Lists other than `touched` as they were, every chain whole,
        // the dangling entries the fixture's.
        const auto othersUnchanged = [&](std::int64_t touched) {
            const auto e = entryChains(rdb);
            for (const auto &[listId, order] : entriesBefore) {
                if (listId != touched) {
                    assert(e.count(listId) && e.at(listId) == order);
                }
            }
            assert(e.size() == entriesBefore.size() || (e.size() + 1 == entriesBefore.size() && !e.count(touched)));
            assert(playlistChains(rdb) == playlistsBefore);
            assert(dangling(lib) == danglingBefore);
            return e;
        };

        // A mid-list member goes; the other 99 keep their order.
        const std::int64_t victim = members.at(42);
        std::vector<std::int64_t> expected = members;
        expected.erase(expected.begin() + 42);
        bool removed = false;
        try {
            removed = removeFromEnginePlaylist(lib, "Playlist 000", victim);
        } catch (const std::exception &e) {
            std::cerr << "remove mid-list member " << victim << " threw: " << e.what() << "\n";
        }
        assert(removed && "the mid-list member is removed");
        {
            const auto e = othersUnchanged(231);
            assert(e.at(231) == expected && "the other 99 remain, in order (walked both ways)");
            assert(djinteropTracks(lib, {"Playlist 000"}) == expected);
        }
        assert(!removeFromEnginePlaylist(lib, "Playlist 000", victim));
        othersUnchanged(231);

        // An entry whose own id is another member's track id. Planted:
        // track `outsider` appended to Playlist 000 as an entry with id
        // `coincidence`, a track id already in the list. Removing track
        // `coincidence` must take its own entry, not the planted one.
        const std::int64_t coincidence = expected.at(10);
        std::int64_t outsider = 0;
        for (const auto &r : rows(rdb, "SELECT id FROM Track ORDER BY id;")) {
            if (std::find(expected.begin(), expected.end(), r[0]) == expected.end()) {
                outsider = r[0];
                break;
            }
        }
        assert(outsider > 0);
        assert(scalar(rdb, "SELECT count(*) FROM PlaylistEntity WHERE id = " + std::to_string(coincidence) + ";") == 0);
        assert(exec(rdb, "INSERT INTO PlaylistEntity (id, listId, trackId, databaseUuid, nextEntityId, "
                         "membershipReference) SELECT "
                             + std::to_string(coincidence) + ", 231, " + std::to_string(outsider)
                             + ", databaseUuid, 0, 0 FROM PlaylistEntity WHERE listId = 231 LIMIT 1;"));
        assert(exec(rdb, "UPDATE PlaylistEntity SET nextEntityId = " + std::to_string(coincidence)
                             + " WHERE listId = 231 AND nextEntityId = 0 AND id <> " + std::to_string(coincidence)
                             + ";"));
        expected.push_back(outsider);
        assert(entryChains(rdb).at(231) == expected);

        removed = false;
        try {
            removed = removeFromEnginePlaylist(lib, "Playlist 000", coincidence);
        } catch (const std::exception &e) {
            std::cerr << "remove member " << coincidence << " threw: " << e.what() << "\n";
        }
        const auto after = entryChains(rdb).at(231);
        const bool plantedStays = std::find(after.begin(), after.end(), outsider) != after.end();
        std::cout << "  planted entry id=" << coincidence << " (track " << outsider << ") "
                  << (plantedStays ? "stays" : "was deleted") << "\n";
        assert(plantedStays && "the entry whose id coincides with the track id stays");
        assert(removed && "the member is removed");
        expected.erase(std::find(expected.begin(), expected.end(), coincidence));
        {
            const auto e = othersUnchanged(231);
            assert(e.at(231) == expected);
            assert(djinteropTracks(lib, {"Playlist 000"}) == expected);
        }
        assert(scalar(rdb, "SELECT trackId FROM PlaylistEntity WHERE id = " + std::to_string(coincidence) + ";")
               == outsider);

        // Head and tail too.
        assert(removeFromEnginePlaylist(lib, "Playlist 000", expected.front()));
        expected.erase(expected.begin());
        assert(removeFromEnginePlaylist(lib, "Playlist 000", expected.back()));
        expected.pop_back();
        assert(othersUnchanged(231).at(231) == expected);
        assert(djinteropTracks(lib, {"Playlist 000"}) == expected);

        // Refusals change nothing.
        assert(throws([&] { removeFromEnginePlaylist(lib, "Playlist 000", 999999); }, "no track"));
        assert(throws([&] { removeFromEnginePlaylist(lib, "Playlist 404", expected.front()); }, "no list"));
        assert(othersUnchanged(231).at(231) == expected);
    }

    std::cout << "engine_playlists_test: all passed\n";
    return 0;
}
