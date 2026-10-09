// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// CreateEnginePlaylistChange, RenameEnginePlaylistChange and the Engine-only
// DeletePlaylistChange through the save loop, on a copy of the anonymized
// fixture with both catalogs, its import counter levelled first. The
// fixture's Engine library has 33 root playlists, "Playlist 000" to
// "Playlist 032", and no folders.
//
// 1. One save in the plan's order: create "Sync Folder/New List" (and so
//    the folder), create "Playlist 003" (there already: a skip), move
//    "Playlist 005" into the new folder as "Moved", rename "Playlist 006"
//    in place, delete "Playlist 007". The moved and renamed playlists keep
//    their ids and their entries in order; the roots are 33 + 1 - 2 = 32,
//    "Playlist 006 renamed" where "Playlist 006" was.
// 2. The same rename again: already done, a skip.
// 3. Undo: m.db byte for byte as it was.
// 4. A path Engine spells twice ("Twice/Child", a folder's child and a
//    root titled with a "/"): create, rename and delete all refuse it,
//    nothing written.

#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>

#include <djinterop/djinterop.hpp>

#include "gui/edit/changes/engine_playlist_changes.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "engine_change_fixture.hpp"

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;

namespace
{

std::int64_t playlistId(const std::string &db, const std::string &title, const std::string &parent = "0")
{
    const auto r = testing::sqlRows(db, "SELECT id FROM Playlist WHERE parentListId = " + parent + " AND title = '"
                                            + title + "';");
    assert(r.size() == 1);
    return std::stoll(r[0][0]);
}

// The titles of a parent's children in nextListId order.
std::vector<std::string> children(const std::string &db, std::int64_t parent)
{
    std::map<std::int64_t, std::pair<std::string, std::int64_t>> rows;  // id -> (title, next)
    std::set<std::int64_t> pointedAt;
    for (const auto &r : testing::sqlRows(db, "SELECT id, title, nextListId FROM Playlist WHERE parentListId = "
                                                  + std::to_string(parent) + ";")) {
        rows[std::stoll(r[0])] = {r[1], std::stoll(r[2])};
        pointedAt.insert(std::stoll(r[2]));
    }
    std::vector<std::string> order;
    for (const auto &[id, row] : rows) {
        if (!pointedAt.count(id)) {
            for (std::int64_t at = id; at != 0; at = rows.at(at).second) {
                order.push_back(rows.at(at).first);
            }
        }
    }
    assert(order.size() == rows.size() && "one chain through every child");
    return order;
}

std::vector<std::int64_t> entriesOf(const std::string &db, std::int64_t listId)
{
    // Every entry of the list in chain order, by its own walk.
    std::map<std::int64_t, std::pair<std::int64_t, std::int64_t>> e;
    std::set<std::int64_t> pointedAt;
    for (const auto &r : testing::sqlRows(db, "SELECT id, trackId, nextEntityId FROM PlaylistEntity WHERE listId = "
                                                  + std::to_string(listId) + ";")) {
        e[std::stoll(r[0])] = {std::stoll(r[1]), std::stoll(r[2])};
        pointedAt.insert(std::stoll(r[2]));
    }
    std::vector<std::int64_t> order;
    for (const auto &[id, row] : e) {
        if (!pointedAt.count(id)) {
            for (std::int64_t at = id; at != 0; at = e.at(at).second) {
                order.push_back(e.at(at).first);
            }
        }
    }
    assert(order.size() == e.size());
    return order;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: engine_playlist_changes_test <tests/fixtures/anonymized_library>\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    const fs::path fixture = pathFromUtf8(argv[1]);
    testing::sandboxSeabassHome(testing::scratchRoot() / "engine_playlist_changes_home");
    const testing::EngineChangeStick stick = testing::makeEngineChangeStick(fixture, "engine_playlist_changes_stick");
    testing::levelImportCounter(stick);
    const std::string library = stick.engineUtf8();
    const std::string originalBytes = testing::fileBytes(pathFromUtf8(stick.db));

    const auto roots = children(stick.db, 0);
    assert(roots.size() == 33 && roots.front() == "Playlist 000" && roots.back() == "Playlist 032");
    const std::int64_t id005 = playlistId(stick.db, "Playlist 005");
    const std::int64_t id006 = playlistId(stick.db, "Playlist 006");
    const auto entries005 = entriesOf(stick.db, id005);
    const auto entries006 = entriesOf(stick.db, id006);
    assert(!entries005.empty() && !entries006.empty());
    const std::int64_t entriesBefore = testing::sqlScalar(stick.db, "SELECT count(*) FROM PlaylistEntity;");
    const std::int64_t entries007 = testing::sqlScalar(
        stick.db, "SELECT count(*) FROM PlaylistEntity WHERE listId = " + std::to_string(playlistId(stick.db, "Playlist 007")) + ";");

    // 1. One save, the plan's order.
    auto createList = std::make_shared<CreateEnginePlaylistChange>(stick.enginePath(), "Sync Folder/New List", 5);
    auto createThere = std::make_shared<CreateEnginePlaylistChange>(stick.enginePath(), "Playlist 003", 5);
    auto move = std::make_shared<RenameEnginePlaylistChange>(stick.enginePath(), "Playlist 005", "Sync Folder/Moved", 5);
    auto rename = std::make_shared<RenameEnginePlaylistChange>(stick.enginePath(), "Playlist 006", "Playlist 006 renamed", 5);
    std::shared_ptr<PendingChange> remove(deleteEnginePlaylistChange(stick.enginePath(), "Playlist 007"));
    assert(createList->id() == QStringLiteral("rekordbox-export-sync:engine-playlist-create:Sync Folder/New List"));
    assert(move->id() == QStringLiteral("rekordbox-export-sync:engine-playlist-rename:Playlist 005"));
    assert(remove->id() == QStringLiteral("rekordbox-export-sync:addcue:delete-playlist:Playlist 007"));
    for (const PendingChange *c : {static_cast<PendingChange *>(createList.get()), static_cast<PendingChange *>(move.get()),
                                   remove.get()}) {
        assert(c->owner() == QStringLiteral("rekordbox-export-sync"));
        assert(c->unit() == QStringLiteral("playlists"));
        assert(c->formatsTouched() == QStringList{QStringLiteral("engine")});
    }
    assert(createList->verb() == QStringLiteral("created") && move->verb() == QStringLiteral("renamed")
           && remove->verb() == QStringLiteral("deleted"));
    assert(move->description() == QStringLiteral("Rename the playlist \"Playlist 005\" to \"Sync Folder/Moved\" in Engine"));
    const SaveLoopResult saved =
        testing::saveChanges({createList, createThere, move, rename, remove}, stick.pioneerPath(), stick.enginePath());
    if (!saved.error.isEmpty()) {
        std::cerr << "save: " << saved.error.toStdString() << "\n";
    }
    assert(saved.error.isEmpty() && saved.warning.isEmpty());
    assert(saved.appliedIds.size() == 5);
    assert(saved.skippedIds == QStringList{createThere->id()} && "\"Playlist 003\" was there already");

    using infrastructure::engine::enginePlaylistCountAtPath;
    assert(enginePlaylistCountAtPath(library, "Sync Folder") == 1);
    assert(enginePlaylistCountAtPath(library, "Sync Folder/New List") == 1);
    assert(enginePlaylistCountAtPath(library, "Sync Folder/Moved") == 1);
    assert(enginePlaylistCountAtPath(library, "Playlist 005") == 0);
    assert(enginePlaylistCountAtPath(library, "Playlist 006") == 0);
    assert(enginePlaylistCountAtPath(library, "Playlist 006 renamed") == 1);
    assert(enginePlaylistCountAtPath(library, "Playlist 007") == 0);
    const std::int64_t folder = playlistId(stick.db, "Sync Folder");
    assert((children(stick.db, folder) == std::vector<std::string>{"New List", "Moved"}) && "moved in last");
    assert(playlistId(stick.db, "Moved", std::to_string(folder)) == id005 && "a move keeps the id");
    assert(playlistId(stick.db, "Playlist 006 renamed") == id006 && "a rename keeps the id");
    assert(entriesOf(stick.db, id005) == entries005 && entriesOf(stick.db, id006) == entries006);
    std::vector<std::string> expectedRoots;
    for (const auto &r : roots) {
        if (r == "Playlist 005" || r == "Playlist 007") {
            continue;
        }
        expectedRoots.push_back(r == "Playlist 006" ? "Playlist 006 renamed" : r);
    }
    expectedRoots.push_back("Sync Folder");
    assert(children(stick.db, 0) == expectedRoots && expectedRoots.size() == 32);
    assert(testing::sqlScalar(stick.db, "SELECT count(*) FROM PlaylistEntity;") == entriesBefore - entries007
           && "only the deleted playlist's entries went");
    std::cout << "case 1 (create with its folder, an existing one skipped, move, rename, delete in one save) OK\n";

    // 2. Already renamed: a skip, nothing written.
    {
        const std::string now = testing::fileBytes(pathFromUtf8(stick.db));
        auto again = std::make_shared<RenameEnginePlaylistChange>(stick.enginePath(), "Playlist 005", "Sync Folder/Moved", 1);
        const SaveLoopResult skipped = testing::saveChanges({again}, stick.pioneerPath(), stick.enginePath());
        assert(skipped.error.isEmpty() && skipped.skippedIds == QStringList{again->id()});
        assert(testing::fileBytes(pathFromUtf8(stick.db)) == now);
        std::cout << "case 2 (a rename already done is a skip) OK\n";
    }

    // 3. Undo.
    {
        const SaveLoopResult undone = testing::undoSave(saved, stick.pioneerPath(), stick.enginePath());
        assert(undone.error.isEmpty() && undone.appliedIds.size() == 1);
        assert(testing::fileBytes(pathFromUtf8(stick.db)) == originalBytes && "Undo puts m.db back byte for byte");
        assert(children(stick.db, 0) == roots);
        std::cout << "case 3 (Undo: m.db byte for byte) OK\n";
    }

    // 4. One path, two playlists: every change refuses it.
    {
        (void)infrastructure::engine::createEnginePlaylist(library, "Twice/Child");
        {
            auto db = djinterop::engine::load_database(library);
            db.create_root_playlist("Twice/Child");
        }
        assert(enginePlaylistCountAtPath(library, "Twice/Child") == 2);
        const std::string planted = testing::fileBytes(pathFromUtf8(stick.db));
        const auto refusedWith = [&](std::shared_ptr<PendingChange> change, const QString &expect) {
            const SaveLoopResult r = testing::saveChanges({change}, stick.pioneerPath(), stick.enginePath());
            if (r.failedId != change->id() || !r.error.contains(expect)) {
                std::cerr << change->id().toStdString() << ": \"" << r.error.toStdString() << "\"\n";
            }
            assert(r.failedId == change->id() && r.error.contains(expect));
            assert(testing::fileBytes(pathFromUtf8(stick.db)) == planted && "nothing written");
        };
        refusedWith(std::make_shared<CreateEnginePlaylistChange>(stick.enginePath(), "Twice/Child", 1),
                    QStringLiteral("Engine has 2 playlists named \"Twice/Child\""));
        refusedWith(std::make_shared<RenameEnginePlaylistChange>(stick.enginePath(), "Twice/Child", "Once", 1),
                    QStringLiteral("Twice/Child"));
        refusedWith(std::shared_ptr<PendingChange>(deleteEnginePlaylistChange(stick.enginePath(), "Twice/Child")),
                    QStringLiteral("Engine has 2 playlists named \"Twice/Child\""));
        std::cout << "case 4 (a path two playlists spell: create, rename and delete refuse it) OK\n";
    }

    std::cout << "engine_playlist_changes_test: all cases passed\n";
    return 0;
}
