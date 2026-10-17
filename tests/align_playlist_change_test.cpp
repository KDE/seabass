// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Engine's half of the playlist repair (#61), through the real save loop:
// AlignPlaylistChange adds a track to and takes one out of an Engine
// playlist, and RemoveDanglingPlaylistEntriesChange deletes entries that
// name no track while the rest keep their order.
//
// Then the two resolutions of a playlist only some libraries have, on a
// stick with all three (rekordbox's export.pdb from
// tests/fixtures/pdb_playlist_tree/3-folder.pdb, the OneLibrary database
// rekordbox started from, an Engine library built beside them): Follow
// Engine creates the playlist in rekordbox and OneLibrary holding the
// tracks they list, and Library Health's DeletePlaylistChange deletes a
// playlist from every library that has it, tracks kept.

#include <QString>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>

#include <djinterop/djinterop.hpp>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/playlist_sync.hpp"
#include "gui/edit/changes/align_playlist_change.hpp"
#include "gui/edit/changes/playlist_edit_changes.hpp"
#include "gui/edit/changes/remove_dangling_playlist_entries_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "scratch_path.hpp"

// After the project's headers: sqlcipher_dyn.hpp (through the OneLibrary
// writer) declares names sqlite3.h defines as macros.
#include <sqlite3.h>

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

// The files of the three-library stick, under its Contents/, with their
// export.pdb row ids (3-folder.pdb).
const char *const A01 = "Tone Artist 01/Tone Album 01/a01.mp3";
const char *const A02 = "Tone Artist 02/Tone Album 02/a02.mp3";
const char *const A03 = "Tone Artist 03/Tone Album 03/a03.mp3";
// On the stick for Engine only: rekordbox and OneLibrary do not list it.
const char *const X99 = "Engine Only/x99.mp3";

domain::Track catalogRow(const std::string &format, const std::string &id, const std::string &file,
                         std::vector<std::string> playlists = {})
{
    domain::Track t;
    t.format = format;
    t.sourceId = id;
    t.filePath = file;
    t.title = file;
    for (auto &p : playlists) {
        t.playlists.push_back({p, -1});
    }
    return t;
}

std::vector<uint32_t> pdbMembers(const fs::path &pioneer, const std::string &playlist)
{
    const auto ids = infrastructure::rekordbox::rekordboxPlaylistIdsByPath(pathToUtf8(pioneer));
    const auto it = ids.find(playlist);
    if (it == ids.end()) {
        return {};
    }
    return infrastructure::rekordbox::PdbRowWriter(pathToUtf8(pioneer / "rekordbox" / "export.pdb"))
        .playlistTrackIds(it->second);
}

// OneLibrary's playlist as the files its rows list, in order; empty when
// there is no such playlist.
std::vector<std::string> oneLibraryMembers(const fs::path &pioneer, const std::string &playlist,
                                           const std::vector<std::string> &files)
{
    infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(pioneer));
    if (w.playlistCountAtPath(playlist) == 0) {
        return {};
    }
    std::map<int64_t, std::string> fileOf;
    for (const auto &f : files) {
        try {
            for (const auto id : w.contentIdsForFile(f)) {
                fileOf[id] = f;
            }
        } catch (const infrastructure::onelibrary::OneLibraryRowMissing &) {
        }
    }
    std::vector<std::string> out;
    for (const auto &[contentId, sequence] : w.playlistContent(playlist)) {
        out.push_back(fileOf.contains(contentId) ? fileOf.at(contentId) : "?");
    }
    return out;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "usage: align_playlist_change_test <pdb_playlist_tree dir> <onelibrary_playlist_tree dir>\n";
        return 2;
    }
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

    // The three-library stick.
    const fs::path pdbFixtures = pathFromUtf8(argv[1]);
    const fs::path olFixture = pathFromUtf8(argv[2]);
    const fs::path three = testing::scratchRoot() / "seabass_align_playlist_change_test_three";
    fs::remove_all(three);
    const fs::path pioneer = three / "PIONEER";
    const fs::path engine = three / "Engine Library";
    fs::create_directories(pioneer / "rekordbox");
    fs::copy_file(pdbFixtures / "3-folder.pdb", pioneer / "rekordbox" / "export.pdb");
    for (const char *name : {"exportLibrary.db", "exportLibrary.db-wal"}) {
        fs::copy_file(olFixture / "0-base" / "PIONEER" / "rekordbox" / name, pioneer / "rekordbox" / name);
    }
    const auto file = [&](const char *relative) { return pathToUtf8(three / "Contents" / relative); };
    std::map<std::string, std::int64_t> engineId;
    {
        auto db = djinterop::engine::create_database(pathToUtf8(engine));
        auto solo = db.create_root_playlist("Solo");
        auto both = db.create_root_playlist("Both");
        auto sub = db.create_root_playlist("F1").create_sub_playlist("Sub");
        for (const char *f : {A01, A02, A03, X99}) {
            djinterop::track_snapshot s;
            s.relative_path = std::string("../Contents/") + f;
            auto t = db.create_track(s);
            engineId[f] = t.id();
            if (f != A03) {
                solo.add_track_back(t);
            }
            if (f == A03) {
                both.add_track_back(t);
            }
            if (f == A01) {
                sub.add_track_back(t);
            }
        }
    }
    {
        infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(pioneer));
        w.createPlaylist("", "Both", false);
        w.addToPlaylist("Both", file(A03));
        // 3-folder.pdb has the folder F1; OneLibrary gets it here.
        w.createPlaylist("", "F1", true);
    }
    const QString pioneerQ = pathToQString(pioneer);
    const QString engineQ = pathToQString(engine);
    const auto save = [&](std::shared_ptr<PendingChange> change) {
        SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, {}, engineQ);
        const SaveLoopResult result = runSaveLoop({change}, ctx);
        assert(result.error.isEmpty());
        return result;
    };

    // 3. "Solo" is in Engine only, with a01, a02 and x99; rekordbox and
    //    OneLibrary list a01 and a02, not x99. The alignment comes from
    //    the domain over catalogs as the scan reads them.
    {
        const std::vector<domain::PlaylistCatalog> catalogs = {
            {"rekordbox", {catalogRow("rekordbox", "1", file(A01)), catalogRow("rekordbox", "2", file(A02)),
                           catalogRow("rekordbox", "3", file(A03))}},
            {"onelibrary", {catalogRow("onelibrary", "101", file(A01)), catalogRow("onelibrary", "102", file(A02)),
                            catalogRow("onelibrary", "103", file(A03), {"Both"})}},
            {"engine", {catalogRow("engine", std::to_string(engineId[A01]), file(A01), {"Solo"}),
                        catalogRow("engine", std::to_string(engineId[A02]), file(A02), {"Solo"}),
                        catalogRow("engine", std::to_string(engineId[A03]), file(A03), {"Both"}),
                        catalogRow("engine", std::to_string(engineId[X99]), file(X99), {"Solo"})}},
        };
        const auto key = [](const std::string &f) { return f; };
        const auto differences = domain::findPlaylistDifferences(catalogs, key);
        const domain::PlaylistDifference *solo = nullptr;
        for (const auto &d : differences) {
            solo = d.name == "Solo" ? &d : solo;
        }
        assert(solo != nullptr && solo->missingSomewhere());
        const auto alignments = domain::alignTo(*solo, "engine", catalogs, key);
        assert(alignments.size() == 2);
        assert(pdbMembers(pioneer, "Solo").empty() && "rekordbox has no Solo yet");
        const auto change = std::make_shared<AlignPlaylistChange>(pioneerQ, engineQ, "Solo", "engine", alignments, 1);
        assert((change->formatsTouched() == QStringList{QStringLiteral("rekordbox"), QStringLiteral("onelibrary")}));
        const auto result = save(change);
        assert(result.appliedIds.size() == 1 && result.skippedIds.isEmpty());
        assert((pdbMembers(pioneer, "Solo") == std::vector<uint32_t>{1, 2}) && "rekordbox: created, a01 and a02");
        const std::vector<std::string> files{file(A01), file(A02), file(A03), file(X99)};
        assert((oneLibraryMembers(pioneer, "Solo", files) == std::vector<std::string>{file(A01), file(A02)})
               && "OneLibrary: created, a01 and a02");
        assert((members(engine, "Solo") == std::vector<std::int64_t>{engineId[A01], engineId[A02], engineId[X99]})
               && "Engine unchanged");
        // Saved again, everything is there already: a skip, no second
        // playlist and no entry twice.
        const auto again = save(std::make_shared<AlignPlaylistChange>(pioneerQ, engineQ, "Solo", "engine", alignments, 1));
        assert(again.skippedIds.size() == 1 && "nothing to do is a skip");
        assert((pdbMembers(pioneer, "Solo") == std::vector<uint32_t>{1, 2}));
        assert(oneLibraryMembers(pioneer, "Solo", files).size() == 2);
        assert(infrastructure::onelibrary::OneLibraryCueWriter(pathToUtf8(pioneer)).playlistCountAtPath("Solo") == 1);
        assert(infrastructure::rekordbox::rekordboxPlaylistIdsByPath(pathToUtf8(pioneer)).count("Solo") == 1);
    }
    std::cout << "case 3 (Follow Engine creates the playlist where it is missing, with what each library lists) OK\n";

    // 3b. "F1/Sub", in Engine only, is created in the folder F1 the other
    //     two have.
    {
        domain::Track a01 = catalogRow("rekordbox", "1", file(A01));
        domain::PlaylistAlignment toPdb;
        toPdb.format = "rekordbox";
        toPdb.create = true;
        toPdb.add = {a01};
        domain::PlaylistAlignment toOneLibrary = toPdb;
        toOneLibrary.format = "onelibrary";
        toOneLibrary.add[0].format = "onelibrary";
        const auto result = save(std::make_shared<AlignPlaylistChange>(
            pioneerQ, engineQ, "F1/Sub", "engine", std::vector<domain::PlaylistAlignment>{toPdb, toOneLibrary}, 1));
        assert(result.appliedIds.size() == 1 && result.skippedIds.isEmpty());
        assert((pdbMembers(pioneer, "F1/Sub") == std::vector<uint32_t>{1}) && "export.pdb: in the folder F1");
        assert((oneLibraryMembers(pioneer, "F1/Sub", {file(A01)}) == std::vector<std::string>{file(A01)})
               && "OneLibrary: in the folder F1");
    }
    std::cout << "case 3b (a playlist in a folder is created in that folder) OK\n";

    // 4. "Both" is in OneLibrary and Engine (a03), not rekordbox. Library
    //    Health deletes it from the two that have it; a03 stays a track.
    {
        const auto change = std::make_shared<DeletePlaylistChange>(
            pioneerQ, engineQ, "Both", QStringList{QStringLiteral("onelibrary"), QStringLiteral("engine")},
            QStringLiteral("library-health"));
        assert(change->owner() == QStringLiteral("library-health"));
        assert(change->id() == DeletePlaylistChange::idFor("Both"));
        assert(DeletePlaylistChange(pioneerQ, engineQ, "Both", {}).owner() == QStringLiteral("addcue")
               && "Browse's stays addcue");
        const auto result = save(change);
        assert(result.appliedIds.size() == 1 && result.skippedIds.isEmpty());
        infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(pioneer));
        assert(w.playlistCountAtPath("Both") == 0 && "out of OneLibrary");
        assert(!w.contentIdsForFile(file(A03)).empty() && "OneLibrary keeps the track");
        auto db = djinterop::engine::load_database(pathToUtf8(engine));
        assert(!db.root_playlist_by_name("Both").has_value() && "out of Engine");
        assert(db.track_by_id(engineId[A03]).has_value() && "Engine keeps the track");
        assert(infrastructure::rekordbox::PdbRowWriter(pathToUtf8(pioneer / "rekordbox" / "export.pdb")).trackExists(3));
    }
    std::cout << "case 4 (Library Health deletes a playlist from every library that has it, tracks kept) OK\n";

    fs::remove_all(three);
    std::cout << "align_playlist_change_test: all cases passed\n";
    return 0;
}
