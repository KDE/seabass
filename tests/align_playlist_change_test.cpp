// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Engine's half of the playlist repair (#61), through the real save loop:
// AlignPlaylistChange adds a track to and takes one out of an Engine
// playlist, and RemoveDanglingPlaylistEntriesChange deletes entries that
// name no track while the rest keep their order.

#include <QString>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>

#include <djinterop/djinterop.hpp>
#include <sqlite3.h>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "gui/edit/changes/align_playlist_change.hpp"
#include "gui/edit/changes/remove_dangling_playlist_entries_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;

namespace
{

std::vector<std::int64_t> members(const fs::path &library, const std::string &name)
{
    auto db = djinterop::engine::load_database(pathToUtf8(library));
    for (const auto &pl : db.root_playlists()) {
        if (pl.name() == name) {
            std::vector<std::int64_t> ids;
            for (const auto &t : pl.tracks()) {
                ids.push_back(t.id());
            }
            return ids;
        }
    }
    return {};
}

domain::Track engineRow(std::int64_t id)
{
    domain::Track t;
    t.format = "engine";
    t.sourceId = std::to_string(id);
    return t;
}

}  // namespace

int main()
{
    const fs::path stick = testing::scratchRoot() / "seabass_align_playlist_change_test";
    fs::remove_all(stick);
    const fs::path library = stick / "Engine Library";
    fs::create_directories(stick);
    std::int64_t a = 0, b = 0, c = 0, gone = 0;
    {
        auto db = djinterop::engine::create_database(pathToUtf8(library));
        auto track = [&db](const char *file) {
            djinterop::track_snapshot s;
            s.relative_path = std::string("../Contents/") + file;
            return db.create_track(s);
        };
        auto ta = track("a.mp3"), tb = track("b.mp3"), tc = track("c.mp3"), tg = track("gone.mp3");
        a = ta.id();
        b = tb.id();
        c = tc.id();
        gone = tg.id();
        auto peak = db.create_root_playlist("Peak");
        peak.add_track_back(ta);
        peak.add_track_back(tg);
        peak.add_track_back(tc);
        db.remove_track(tg);  // leaves Peak's entry for it behind? libdjinterop may tidy it
    }
    // Make sure there is a dangling entry whatever libdjinterop tidied:
    // an entry for a track id the library does not have, mid-list.
    {
        sqlite3 *h = nullptr;
        assert(sqlite3_open(pathToUtf8(library / "Database2" / "m.db").c_str(), &h) == SQLITE_OK);
        const std::string count = "SELECT count(*) FROM PlaylistEntity WHERE trackId = " + std::to_string(gone);
        sqlite3_stmt *st = nullptr;
        sqlite3_prepare_v2(h, count.c_str(), -1, &st, nullptr);
        sqlite3_step(st);
        const int left = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
        if (left == 0) {
            // Insert one between a and c by hand, linked the way Engine links.
            const std::string sql =
                "INSERT INTO PlaylistEntity (listId, trackId, databaseUuid, nextEntityId, membershipReference) "
                "SELECT e.listId, 999999, e.databaseUuid, e.nextEntityId, 0 FROM PlaylistEntity e WHERE e.trackId = "
                + std::to_string(a) + ";"
                "UPDATE PlaylistEntity SET nextEntityId = last_insert_rowid() WHERE trackId = " + std::to_string(a) + ";";
            assert(sqlite3_exec(h, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
        }
        sqlite3_close(h);
    }
    const auto dangling = infrastructure::engine::danglingPlaylistEntries(pathToUtf8(library));
    assert(dangling.size() == 1 && dangling[0].playlist == "Peak" && dangling[0].entries == 1);
    // libdjinterop hands the dead entry back as a track id with no row;
    // the Engine reader drops it, which is why nothing else saw it.
    assert((members(library, "Peak") == std::vector<std::int64_t>{a, gone, c}));
    std::cout << "case 1 (an entry naming no track is found) OK\n";

    application::CancellationToken token;
    {
        SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, {}, pathToQString(library));
        std::vector<std::shared_ptr<PendingChange>> changes;
        changes.push_back(std::make_shared<RemoveDanglingPlaylistEntriesChange>(pathToQString(library), 1));
        domain::PlaylistAlignment alignment;
        alignment.format = "engine";
        alignment.add = {engineRow(b)};
        alignment.remove = {engineRow(a)};
        changes.push_back(std::make_shared<AlignPlaylistChange>(QString(), pathToQString(library), "Peak", "rekordbox",
                                                                std::vector<domain::PlaylistAlignment>{alignment}, 1));
        const SaveLoopResult result = runSaveLoop(changes, ctx);
        assert(result.error.isEmpty() && result.appliedIds.size() == 2);
    }
    assert(infrastructure::engine::danglingPlaylistEntries(pathToUtf8(library)).empty());
    assert((members(library, "Peak") == std::vector<std::int64_t>{c, b}) && "a out, b at the end, c kept");
    std::cout << "case 2 (the dangling entry goes, and the playlist follows the alignment) OK\n";

    fs::remove_all(stick);
    std::cout << "align_playlist_change_test: all cases passed\n";
    return 0;
}
