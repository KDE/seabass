// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Browse's playlist edits through the real save loop, on a stick holding
// the same playlists in all three libraries: rekordbox's own export.pdb
// (tests/fixtures/pdb_playlist_tree/3-folder.pdb: Q1 with a01..a05, folder
// F1 holding F1A with b01 and c01), the OneLibrary database rekordbox
// started from with the same playlists made in it, and an Engine library
// built to match. RemoveFromPlaylistChange takes a03 out of Q1 in all
// three, by path; DeletePlaylistChange deletes F1A from the folder F1 in
// all three, tracks kept, and never the folder.

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <djinterop/djinterop.hpp>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "gui/edit/changes/change_helpers.hpp"
#include "gui/edit/changes/delete_tracks_change.hpp"
#include "gui/edit/changes/playlist_edit_changes.hpp"
#include "gui/edit/format_write_session.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/backup/stick_locks.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/engine/engine_track_rows.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"
#include "infrastructure/paths/utf8_path.hpp"
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

struct File
{
    const char *relative;  // under the stick's Contents/
    uint32_t pdbId;        // the row id in the fixture's export.pdb
};
const File A01{"Tone Artist 01/Tone Album 01/a01.mp3", 1};
const File A02{"Tone Artist 02/Tone Album 02/a02.mp3", 2};
const File A03{"Tone Artist 03/Tone Album 03/a03.mp3", 3};
const File A04{"Tone Artist 04/Tone Album 04/a04.mp3", 4};
const File A05{"Tone Artist 05/Tone Album 01/a05.mp3", 5};
const File B01{"Nouvel Artiste/Nouvel Album/b01.mp3", 13};
const File C01{"Tone Artist 01/Tone Album 01/c01.mp3", 14};
const File A10{"Tone Artist 05/Tone Album 02/a10.mp3", 0};  // in no playlist; pdb id looked up

std::vector<std::int64_t> engineMembers(const fs::path &library, const std::string &path)
{
    auto db = djinterop::engine::load_database(pathToUtf8(library));
    std::optional<djinterop::playlist> current;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        current = current ? current->sub_playlist_by_name(part) : db.root_playlist_by_name(part);
        if (!current) {
            return {-1};
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    std::vector<std::int64_t> ids;
    for (const auto &t : current->tracks()) {
        ids.push_back(t.id());
    }
    return ids;
}

// How many rows of the library's m.db name `trackId`: its Track row plus
// every PerformanceData, PlaylistEntity, PreparelistEntity row.
int engineRowsNaming(const fs::path &library, std::int64_t trackId)
{
    std::string error;
    const int rows = infrastructure::engine::engineRowsNamingTrack(pathToUtf8(library / "Database2" / "m.db"), trackId, &error);
    assert(rows >= 0 && error.empty());
    return rows;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "usage: playlist_edit_changes_test <pdb_playlist_tree dir> <onelibrary_playlist_tree dir>\n";
        return 2;
    }
    const fs::path pdbFixtures = pathFromUtf8(argv[1]);
    const fs::path olFixture = pathFromUtf8(argv[2]);
    const fs::path stick = testing::scratchRoot() / "seabass_playlist_edit_changes_test";
    fs::remove_all(stick);
    const fs::path pioneer = stick / "PIONEER";
    const fs::path library = stick / "Engine Library";
    fs::create_directories(pioneer / "rekordbox");
    fs::copy_file(pdbFixtures / "3-folder.pdb", pioneer / "rekordbox" / "export.pdb");
    for (const char *name : {"exportLibrary.db", "exportLibrary.db-wal"}) {
        fs::copy_file(olFixture / "0-base" / "PIONEER" / "rekordbox" / name, pioneer / "rekordbox" / name);
    }
    const auto file = [&](const File &f) { return pathToUtf8(stick / "Contents" / f.relative); };
    {
        infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(pioneer));
        w.createPlaylist("", "Q1", false);
        for (const File &f : {A01, A02, A03, A04, A05}) {
            w.addToPlaylist("Q1", file(f));
        }
        w.createPlaylist("", "F1", true);
        w.createPlaylist("F1", "F1A", false);
        w.addToPlaylist("F1/F1A", file(B01));
        w.addToPlaylist("F1/F1A", file(C01));
    }
    std::map<std::string, std::int64_t> engineId;
    {
        auto db = djinterop::engine::create_database(pathToUtf8(library));
        auto q1 = db.create_root_playlist("Q1");
        auto f1 = db.create_root_playlist("F1");
        auto f1a = f1.create_sub_playlist("F1A");
        for (const File &f : {A01, A02, A03, A04, A05, B01, C01, A10}) {
            djinterop::track_snapshot s;
            s.relative_path = std::string("../Contents/") + f.relative;
            auto t = db.create_track(s);
            engineId[f.relative] = t.id();
            if (f.pdbId == 0) {
                continue;  // a10: in no playlist
            }
            (f.pdbId <= 5 ? q1 : f1a).add_track_back(t);
        }
    }
    const QString pioneerQ = pathToQString(pioneer);
    const QString libraryQ = pathToQString(library);

    // 1. Which libraries have a playlist, read from the stick.
    const QStringList all{QStringLiteral("rekordbox"), QStringLiteral("onelibrary"), QStringLiteral("engine")};
    assert(playlistWhereabouts(pioneerQ, libraryQ, "Q1").having == all);
    assert(playlistWhereabouts(pioneerQ, libraryQ, "F1/F1A").having == all);
    assert(playlistWhereabouts(pioneerQ, libraryQ, "Nope").having.isEmpty());
    std::cout << "case 1 (the libraries holding a playlist) OK\n";

    // 2. a03 out of Q1, in all three, by path.
    application::CancellationToken token;
    const auto save = [&](std::shared_ptr<PendingChange> change) {
        SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, {}, libraryQ);
        const SaveLoopResult result = runSaveLoop({change}, ctx);
        assert(result.error.isEmpty());
        return result;
    };
    {
        const auto result = save(std::make_shared<RemoveFromPlaylistChange>(pioneerQ, libraryQ, "Q1", file(A03),
                                                                            QStringLiteral("Tone A03"), all));
        assert(result.appliedIds.size() == 1 && result.skippedIds.isEmpty());
    }
    {
        infrastructure::rekordbox::PdbRowWriter rows(pathToUtf8(pioneer / "rekordbox" / "export.pdb"));
        uint32_t q1 = 0;
        for (const auto &n : rows.playlistTree()) {
            q1 = n.name == "Q1" ? n.id : q1;
        }
        // The new order's check refuses anything but the playlist's tracks:
        // a01, a02, a04, a05 is accepted (and changes nothing), so that is
        // exactly what Q1 holds.
        assert(!rows.reorderPlaylist(q1, {1, 2, 4, 5}) && "export.pdb's Q1 is a01, a02, a04, a05");
        infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(pioneer));
        const auto content = w.playlistContent("Q1");
        assert(content.size() == 4 && content[2].second == 3 && "OneLibrary renumbered");
        assert((engineMembers(library, "Q1")
                == std::vector<std::int64_t>{engineId[A01.relative], engineId[A02.relative], engineId[A04.relative],
                                             engineId[A05.relative]}));
    }
    {
        const auto result = save(std::make_shared<RemoveFromPlaylistChange>(pioneerQ, libraryQ, "Q1", file(A03),
                                                                            QStringLiteral("Tone A03"), all));
        assert(result.skippedIds.size() == 1 && "a track no longer there is a skip");
    }
    std::cout << "case 2 (a track out of a playlist in all three libraries) OK\n";

    // 3. The folder F1 is not deleted, nor Engine's F1 while it holds
    //    F1A: a delete takes nothing below the playlist named. F1A is
    //    deleted from all three, then Engine's F1, empty by then; the
    //    tracks stay, and so do the folders.
    {
        const auto f1 = playlistWhereabouts(pioneerQ, libraryQ, "F1");
        assert(f1.having == all);
        assert((f1.folders == QStringList{QStringLiteral("rekordbox"), QStringLiteral("onelibrary")}));
        assert(f1.holdingOthers == all && "each holds F1A");
        auto result = save(std::make_shared<DeletePlaylistChange>(pioneerQ, libraryQ, "F1", all));
        assert(result.skippedIds.size() == 1 && "left alone everywhere: a skip");
        assert(playlistWhereabouts(pioneerQ, libraryQ, "F1/F1A").having == all && "F1A stays in all three");
        result = save(std::make_shared<DeletePlaylistChange>(pioneerQ, libraryQ, "F1/F1A", all));
        assert(result.appliedIds.size() == 1 && result.skippedIds.isEmpty());
        result = save(std::make_shared<DeletePlaylistChange>(pioneerQ, libraryQ, "F1", all));
        assert(result.appliedIds.size() == 1 && result.skippedIds.isEmpty() && "Engine's F1, empty now, is deleted");
    }
    assert((playlistWhereabouts(pioneerQ, libraryQ, "F1").having
            == QStringList{QStringLiteral("rekordbox"), QStringLiteral("onelibrary")})
           && "the folders stay");
    assert(playlistWhereabouts(pioneerQ, libraryQ, "F1/F1A").having.isEmpty());
    {
        infrastructure::rekordbox::PdbRowWriter rows(pathToUtf8(pioneer / "rekordbox" / "export.pdb"));
        assert(rows.trackExists(B01.pdbId) && rows.trackExists(C01.pdbId) && "export.pdb keeps the tracks");
        assert(!rows.trackIdsWithFilePath("/Contents/" + std::string(B01.relative)).empty());
        infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(pioneer));
        assert(w.playlistContentRowsWithoutPlaylist() == 0);
        auto db = djinterop::engine::load_database(pathToUtf8(library));
        assert(db.track_by_id(engineId[B01.relative]).has_value() && "Engine keeps the tracks");
        assert(!db.root_playlist_by_name("F1").has_value());
    }
    assert(playlistWhereabouts(pioneerQ, libraryQ, "Q1").having == all && "the other playlist is untouched");
    std::cout << "case 3 (a playlist in a folder deleted from all three libraries, tracks kept; no folder, and no "
                 "Engine playlist holding another, deleted) OK\n";

    // 4. Deleting tracks that are in no playlist: a10 is in none in any
    //    library, a01 is in Q1. Only a10 goes, from all three, and its file
    //    is put on the pending-deletions list; a01 is left alone.
    {
        const std::string pdbPath = pathToUtf8(pioneer / "rekordbox" / "export.pdb");
        // As the scan hands them over: every library's row with its own
        // path. The rekordbox row's id is deliberately stale (a01's), as
        // after a re-export: the change must go by the path.
        const auto rowsFor = [&](const File &f, const std::string &rekordboxId) {
            std::vector<domain::Track> rows;
            for (const char *format : {"rekordbox", "onelibrary", "engine"}) {
                domain::Track t;
                t.format = format;
                t.filePath = file(f);
                t.sourceId = std::string(format) == "engine" ? std::to_string(engineId[f.relative])
                             : std::string(format) == "rekordbox" ? rekordboxId : "x";
                rows.push_back(t);
            }
            return rows;
        };
        const std::string a01Id = std::to_string(infrastructure::rekordbox::PdbRowWriter(pdbPath)
                                                     .trackIdsWithFilePath("/Contents/" + std::string(A01.relative)).front());
        std::vector<DeleteTracksChange::Entry> entries = {{file(A10), "Tone A10", "Tone Artist 05", rowsFor(A10, a01Id)},
                                                          {file(A01), "Tone A01", "Tone Artist 01", rowsFor(A01, a01Id)}};
        // a10 is its Track row and the PerformanceData row Engine's insert
        // trigger gave it; Delete Tracks has to take both.
        assert(engineRowsNaming(library, engineId[A10.relative]) == 2 && "a10: Track and PerformanceData");
        const auto result = save(std::make_shared<DeleteTracksChange>(pioneerQ, libraryQ, entries));
        assert(result.appliedIds.size() == 1 && result.skippedIds.isEmpty());

        infrastructure::rekordbox::PdbRowWriter rows(pdbPath);
        assert(rows.trackIdsWithFilePath("/Contents/" + std::string(A10.relative)).empty() && "a10 out of export.pdb");
        assert(!rows.trackIdsWithFilePath("/Contents/" + std::string(A01.relative)).empty() && "a01 kept");
        infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(pioneer));
        bool a10Gone = false;
        try {
            w.addToPlaylist("Q1", file(A10));
        } catch (const infrastructure::onelibrary::OneLibraryRowMissing &) {
            a10Gone = true;
        }
        assert(a10Gone && "a10 out of OneLibrary");
        assert(w.isInAnyPlaylist(file(A01)) && "a01 still in OneLibrary's Q1");
        auto db = djinterop::engine::load_database(pathToUtf8(library));
        assert(!db.track_by_id(engineId[A10.relative]).has_value() && "a10 out of Engine");
        assert(db.track_by_id(engineId[A01.relative]).has_value() && "a01 kept in Engine");
        assert(engineRowsNaming(library, engineId[A10.relative]) == 0 && "no PerformanceData row left behind for a10");
        assert(engineRowsNaming(library, engineId[A01.relative]) == 3 && "a01 keeps its rows and its Q1 entry");
        std::ifstream manifest(stick / "Seabass" / "orphaned" / "pending-deletions.jsonl");
        std::stringstream lines;
        lines << manifest.rdbuf();
        assert(lines.str().find("a10.mp3") != std::string::npos && "a10's file waits on the list");
        assert(lines.str().find("a01.mp3") == std::string::npos && "a01's file does not");
    }
    std::cout << "case 4 (tracks in no playlist deleted from all three, the file queued, a listed one kept) OK\n";

    // 5. Engine tracks found by path from a copy of the library (as a save
    //    writing through a scratch copy sees them), relative to the
    //    stick's real library folder.
    {
        const fs::path copy = testing::scratchRoot() / "seabass_playlist_edit_changes_scratch" / "Engine Library";
        fs::remove_all(copy.parent_path());
        fs::create_directories(copy.parent_path());
        fs::copy(library, copy, fs::copy_options::recursive);
        const auto ids = infrastructure::engine::engineTrackIdsForFile(pathToUtf8(copy), pathToUtf8(library), file(A02));
        assert(ids.size() == 1 && ids[0] == engineId[A02.relative] && "found from the copy, by the real folder's path");
        assert(infrastructure::engine::engineTrackIdsForFile(pathToUtf8(copy), pathToUtf8(copy), file(A02)).empty()
               && "relative to the copy itself, nothing matches");
        fs::remove_all(copy.parent_path());
    }
    std::cout << "case 5 (Engine tracks by path from a scratch copy) OK\n";

    // 6. A playlist called "AC/DC" beside a folder "AC" holding "DC": the
    //    path names two, so Browse refuses and a staged change fails
    //    rather than guess.
    {
        {
            auto db = djinterop::engine::load_database(pathToUtf8(library));
            db.create_root_playlist("AC/DC");
            db.create_root_playlist("AC").create_sub_playlist("DC");
        }
        assert(infrastructure::engine::enginePlaylistCountAtPath(pathToUtf8(library), "AC/DC") == 2);
        assert(playlistWhereabouts(pioneerQ, libraryQ, "AC/DC").several == QStringList{QStringLiteral("engine")});
        SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, {}, libraryQ);
        const SaveLoopResult result =
            runSaveLoop({std::make_shared<DeletePlaylistChange>(pioneerQ, libraryQ, "AC/DC", QStringList{QStringLiteral("engine")})}, ctx);
        assert(!result.error.isEmpty() && "an ambiguous path fails, it does not pick one");
        assert(infrastructure::engine::enginePlaylistCountAtPath(pathToUtf8(library), "AC/DC") == 2 && "both still there");
    }
    std::cout << "case 6 (an ambiguous playlist path is refused) OK\n";

    // 7. A new playlist, "Fresh", made empty in all three, at the top
    //    level and last there; asked again, every library has it: a skip.
    {
        assert(playlistWhereabouts(pioneerQ, libraryQ, "").onStick == all);
        assert(playlistWhereabouts(pioneerQ, QString(), "").onStick == (QStringList{QStringLiteral("rekordbox"), QStringLiteral("onelibrary")}));
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Fresh").having.isEmpty());
        const auto result = save(std::make_shared<CreatePlaylistChange>(pioneerQ, libraryQ, "Fresh", all));
        assert(result.appliedIds.size() == 1 && result.skippedIds.isEmpty());
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Fresh").having == all);
        infrastructure::rekordbox::PdbRowWriter rows(pathToUtf8(pioneer / "rekordbox" / "export.pdb"));
        uint32_t lastOrder = 0;
        std::string last;
        for (const auto &n : rows.playlistTree()) {
            if (n.parentId == 0 && n.sortOrder >= lastOrder) {
                lastOrder = n.sortOrder;
                last = n.name;
            }
            assert(n.name != "Fresh" || (!n.isFolder && n.parentId == 0));
        }
        assert(last == "Fresh" && "export.pdb: last at the top level");
        infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(pioneer));
        assert(w.playlistContent("Fresh").empty());
        assert(engineMembers(library, "Fresh").empty());
        const auto again = save(std::make_shared<CreatePlaylistChange>(pioneerQ, libraryQ, "Fresh", all));
        assert(again.skippedIds.size() == 1 && "every library has it: a skip");
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Fresh").several.isEmpty() && "not made twice");
    }
    std::cout << "case 7 (a new playlist made in all three libraries, once) OK\n";

    // 8. a03 (taken out of Q1 in case 2) added back, at the end, in all
    //    three; asked again, a skip. Then a playlist created and filled in
    //    one save, the create staged first.
    {
        const auto result = save(std::make_shared<AddToPlaylistChange>(pioneerQ, libraryQ, "Q1", file(A03),
                                                                       QStringLiteral("Tone A03"), all));
        assert(result.appliedIds.size() == 1 && result.skippedIds.isEmpty());
        infrastructure::rekordbox::PdbRowWriter rows(pathToUtf8(pioneer / "rekordbox" / "export.pdb"));
        uint32_t q1 = 0;
        for (const auto &n : rows.playlistTree()) {
            q1 = n.name == "Q1" ? n.id : q1;
        }
        assert(!rows.reorderPlaylist(q1, {1, 2, 4, 5, 3}) && "export.pdb's Q1 is a01, a02, a04, a05, a03");
        infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(pioneer));
        const auto content = w.playlistContent("Q1");
        assert(content.size() == 5 && content.back().second == 5 && "OneLibrary: last");
        assert((engineMembers(library, "Q1")
                == std::vector<std::int64_t>{engineId[A01.relative], engineId[A02.relative], engineId[A04.relative],
                                             engineId[A05.relative], engineId[A03.relative]}));
        const auto again = save(std::make_shared<AddToPlaylistChange>(pioneerQ, libraryQ, "Q1", file(A03),
                                                                      QStringLiteral("Tone A03"), all));
        assert(again.skippedIds.size() == 1 && "already there: a skip");
        assert(engineMembers(library, "Q1").size() == 5 && w.playlistContent("Q1").size() == 5);

        SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, {}, libraryQ);
        const SaveLoopResult both = runSaveLoop(
            {std::make_shared<CreatePlaylistChange>(pioneerQ, libraryQ, "Fresher", all),
             std::make_shared<AddToPlaylistChange>(pioneerQ, libraryQ, "Fresher", file(A02), QStringLiteral("Tone A02"), all)},
            ctx);
        assert(both.error.isEmpty() && both.appliedIds.size() == 2 && both.skippedIds.isEmpty());
        assert(engineMembers(library, "Fresher") == std::vector<std::int64_t>{engineId[A02.relative]});
        infrastructure::onelibrary::OneLibraryCueWriter w2(pathToUtf8(pioneer));
        assert(w2.playlistContent("Fresher").size() == 1);
        infrastructure::rekordbox::PdbRowWriter rows2(pathToUtf8(pioneer / "rekordbox" / "export.pdb"));
        uint32_t fresher = 0;
        for (const auto &n : rows2.playlistTree()) {
            fresher = n.name == "Fresher" ? n.id : fresher;
        }
        assert(fresher != 0 && !rows2.reorderPlaylist(fresher, {2}) && "export.pdb's Fresher is a02");
    }
    std::cout << "case 8 (a track added to a playlist in all three, also to one made in the same save) OK\n";

    // 9. Moving a track within Q1 (a01, a02, a04, a05, a03 after case 8)
    //    in all three: to the start, to the end, to the middle; the same
    //    move again and a file Q1 does not hold are skips, each library
    //    saying why. The order is read back from each library itself.
    {
        const std::string pdbPath = pathToUtf8(pioneer / "rekordbox" / "export.pdb");
        const auto pdbIdOf = [&](const File &f) {
            return infrastructure::rekordbox::PdbRowWriter(pdbPath).trackIdsWithFilePath("/Contents/" + std::string(f.relative)).front();
        };
        const auto assertOrder = [&](const std::vector<File> &files, const char *what) {
            infrastructure::rekordbox::PdbRowWriter rows(pdbPath);
            uint32_t q1 = 0;
            for (const auto &n : rows.playlistTree()) {
                q1 = n.name == "Q1" ? n.id : q1;
            }
            std::vector<uint32_t> pdbWanted;
            std::vector<int64_t> olWanted;
            std::vector<std::int64_t> engineWanted;
            infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(pioneer));
            for (const File &f : files) {
                pdbWanted.push_back(pdbIdOf(f));
                olWanted.push_back(w.contentIdsForFile(file(f)).front());
                engineWanted.push_back(engineId[f.relative]);
            }
            if (rows.playlistTrackIds(q1) != pdbWanted) {
                std::cerr << what << ": export.pdb's Q1 is not in the order asked\n";
                assert(false);
            }
            std::vector<int64_t> olOrder;
            std::vector<int64_t> sequence;
            for (const auto &[content, seq] : w.playlistContent("Q1")) {
                olOrder.push_back(content);
                sequence.push_back(seq);
            }
            if (olOrder != olWanted) {
                std::cerr << what << ": OneLibrary's Q1 is not in the order asked\n";
                assert(false);
            }
            assert((sequence == std::vector<int64_t>{1, 2, 3, 4, 5}) && "OneLibrary numbered from 1 again");
            if (engineMembers(library, "Q1") != engineWanted) {
                std::cerr << what << ": Engine's Q1 is not in the order asked\n";
                assert(false);
            }
        };
        const std::string logFile = infrastructure::backup::operationLogForStickRoot(
            infrastructure::backup::stickRootForCatalogPath(pathToUtf8(library)));
        const auto logSince = [&](std::streamoff from) {
            std::ifstream in(pathFromUtf8(logFile));
            std::stringstream all;
            all << in.rdbuf();
            return all.str().substr(std::min<size_t>(static_cast<size_t>(from), all.str().size()));
        };
        const auto logSize = [&]() { return static_cast<std::streamoff>(logSince(0).size()); };
        const auto move = [&](const File &f, int toIndex) {
            return std::make_shared<MoveInPlaylistChange>(pioneerQ, libraryQ, "Q1", file(f), QStringLiteral("Tone"), toIndex, all);
        };

        assertOrder({A01, A02, A04, A05, A03}, "before");
        {
            SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, {}, libraryQ);
            const auto targets = move(A03, 0)->filesToBackup(ctx);
            assert(targets.size() == 3 && "every library it writes is backed up");
#if defined(_WIN32)
            // One spelling per target: a root handed over with '/' and joined with
            // fs::path's '\' read C:/x/PIONEER\rekordbox\export.pdb.
            for (const auto &target : targets) {
                assert((target.file.find('/') == std::string::npos || target.file.find('\\') == std::string::npos)
                       && "a backup target is spelled with one kind of separator");
            }
#endif
            assert(targets[0].file == pdbPath && targets[0].label == "playlist-move");
            assert(targets[1].file == infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pathToUtf8(pioneer)));
            assert(targets[2].file == pathToUtf8(library / "Database2" / "m.db") && targets[2].label == "playlist-move");
            assert(move(A03, 0)->id() == QStringLiteral("addcue:move-in-playlist:Q1|") + QString::fromStdString(file(A03)));
            assert(move(A03, 0)->id() == move(A03, 3)->id() && "a second move of the file replaces the first");
        }

        std::streamoff mark = logSize();
        auto result = save(move(A03, 0));
        assert(result.appliedIds.size() == 1 && result.skippedIds.isEmpty());
        assertOrder({A03, A01, A02, A04, A05}, "to the start");
        assert(logSince(mark).find("playlist-move: moved \"" + file(A03) + "\" to position 1 in \"Q1\" in 3 libraries")
               != std::string::npos);

        result = save(move(A03, 4));
        assert(result.appliedIds.size() == 1 && result.skippedIds.isEmpty());
        assertOrder({A01, A02, A04, A05, A03}, "to the end");

        result = save(move(A01, 2));
        assert(result.appliedIds.size() == 1);
        assertOrder({A02, A04, A01, A05, A03}, "to the middle");

        result = save(move(A05, 99));
        assert(result.appliedIds.size() == 1);
        assertOrder({A02, A04, A01, A03, A05}, "past the end: last");

        mark = logSize();
        result = save(move(A01, 2));
        const std::string noop = logSince(mark);
        assert(result.skippedIds.size() == 1 && "already there: a skip");
        assertOrder({A02, A04, A01, A03, A05}, "a move to where it is");
        for (const char *library : {"rekordbox", "OneLibrary", "Engine"}) {
            assert(noop.find("not moved in \"Q1\" there: " + std::string(library) + " has it there already") != std::string::npos);
        }
        assert(noop.find("playlist-move: \"" + file(A01) + "\" moved in \"Q1\" nowhere, skipped") != std::string::npos);

        mark = logSize();
        result = save(move(B01, 0));
        assert(result.skippedIds.size() == 1 && "not in the playlist: a skip");
        assertOrder({A02, A04, A01, A03, A05}, "a file Q1 does not hold");
        const std::string absent = logSince(mark);
        for (const char *library : {"rekordbox", "OneLibrary", "Engine"}) {
            assert(absent.find("\"" + file(B01) + "\" not moved in \"Q1\" there: " + std::string(library)
                               + "'s playlist does not hold the file")
                   != std::string::npos);
        }

        // Only the libraries named are written: Engine keeps its order.
        result = save(std::make_shared<MoveInPlaylistChange>(pioneerQ, libraryQ, "Q1", file(A05), QStringLiteral("Tone"), 0,
                                                            QStringList{QStringLiteral("rekordbox"), QStringLiteral("onelibrary")}));
        assert(result.appliedIds.size() == 1);
        assert((engineMembers(library, "Q1")
                == std::vector<std::int64_t>{engineId[A02.relative], engineId[A04.relative], engineId[A01.relative],
                                             engineId[A03.relative], engineId[A05.relative]}));
        result = save(move(A05, 0));
        assertOrder({A05, A02, A04, A01, A03}, "level again");

        // 9b. An add at a place (a track dropped between two rows): one
        //     change, the entry added and put there in each library. a03
        //     is taken out first each time, then added back at the start,
        //     in the middle, and past the end (last).
        const auto placedAdd = [&](const File &f, int toIndex) {
            return std::make_shared<AddToPlaylistChange>(pioneerQ, libraryQ, "Q1", file(f), QStringLiteral("Tone"), all,
                                                         toIndex);
        };
        const auto takeOut = [&](const File &f) {
            const auto out = save(std::make_shared<RemoveFromPlaylistChange>(pioneerQ, libraryQ, "Q1", file(f),
                                                                             QStringLiteral("Tone"), all));
            assert(out.appliedIds.size() == 1);
        };
        assert(placedAdd(A03, 2)->id() == AddToPlaylistChange::idFor("Q1", file(A03))
               && "a placed add and a plain one for the file replace each other");
        assert(placedAdd(A03, 2)->description() == QStringLiteral("Add \"Tone\" to \"Q1\" at position 3"));
        assert(placedAdd(A03, 2)->unit() == QStringLiteral("playlist entries") && placedAdd(A03, 2)->verb() == "added");
        {
            SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, {}, libraryQ);
            assert(placedAdd(A03, 2)->filesToBackup(ctx).size() == 3 && "every library it writes is backed up");
        }

        takeOut(A03);
        mark = logSize();
        result = save(placedAdd(A03, 0));
        assert(result.appliedIds.size() == 1 && result.skippedIds.isEmpty());
        assertOrder({A03, A05, A02, A04, A01}, "added at the start");
        assert(logSince(mark).find("playlist-add: added \"" + file(A03) + "\" to \"Q1\" at position 1 in 3 libraries")
               != std::string::npos);

        takeOut(A03);
        result = save(placedAdd(A03, 2));
        assert(result.appliedIds.size() == 1);
        assertOrder({A05, A02, A03, A04, A01}, "added in the middle");

        takeOut(A03);
        result = save(placedAdd(A03, 99));
        assert(result.appliedIds.size() == 1);
        assertOrder({A05, A02, A04, A01, A03}, "added past the end: last");

        // A library already holding the file is left alone, place and all.
        result = save(placedAdd(A01, 0));
        assert(result.skippedIds.size() == 1 && "already there: a skip");
        assertOrder({A05, A02, A04, A01, A03}, "not moved by an add left alone");
    }
    std::cout << "case 9 (a track moved within a playlist, and added at a place, in all three libraries) OK\n";

    // 10. Renaming: Q1 to "Q One" in all three, read back by path with its
    //     tracks in their order; a name taken, an ambiguous path and an
    //     Engine library with a smart playlist filed by the name are left
    //     alone, each said; a folder takes its children's paths along; a
    //     name with "/" fails.
    {
        const std::string pdbPath = pathToUtf8(pioneer / "rekordbox" / "export.pdb");
        const std::string mdb = pathToUtf8(library / "Database2" / "m.db");
        const std::string logFile = infrastructure::backup::operationLogForStickRoot(
            infrastructure::backup::stickRootForCatalogPath(pathToUtf8(library)));
        const auto logText = [&]() {
            std::ifstream in(pathFromUtf8(logFile));
            std::stringstream all;
            all << in.rdbuf();
            return all.str();
        };
        const auto rename = [&](const std::string &from, const std::string &to, const QStringList &libraries) {
            return std::make_shared<RenamePlaylistChange>(pioneerQ, libraryQ, from, to, libraries);
        };
        // Each library's tracks of the playlist at `path`, in its order:
        // export.pdb's track ids, OneLibrary's content ids, Engine's ids.
        const auto orderIn = [&](const std::string &path) {
            infrastructure::rekordbox::PdbRowWriter rows(pdbPath);
            std::vector<uint32_t> pdb;
            for (const auto &n : rows.playlistTree()) {
                if (n.name == path) {
                    pdb = rows.playlistTrackIds(n.id);
                }
            }
            infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(pioneer));
            std::vector<int64_t> ol;
            for (const auto &[content, seq] : w.playlistContent(path)) {
                ol.push_back(content);
            }
            return std::make_tuple(pdb, ol, engineMembers(library, path));
        };

        const auto before = orderIn("Q1");
        assert(std::get<0>(before).size() == 5 && std::get<1>(before).size() == 5 && std::get<2>(before).size() == 5);
        {
            SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, {}, libraryQ);
            const auto targets = rename("Q1", "Q One", all)->filesToBackup(ctx);
            assert(targets.size() == 3 && "every library it writes is backed up");
#if defined(_WIN32)
            // One spelling per target: a root handed over with '/' and joined with
            // fs::path's '\' read C:/x/PIONEER\rekordbox\export.pdb.
            for (const auto &target : targets) {
                assert((target.file.find('/') == std::string::npos || target.file.find('\\') == std::string::npos)
                       && "a backup target is spelled with one kind of separator");
            }
#endif
            assert(targets[0].file == pdbPath && targets[0].label == "playlist-rename");
            assert(targets[1].file == infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pathToUtf8(pioneer)));
            assert(targets[2].file == mdb && targets[2].label == "playlist-rename");
            assert(rename("Q1", "Q One", all)->id() == QStringLiteral("addcue:rename-playlist:Q1"));
        }
        auto result = save(rename("Q1", "Q One", all));
        assert(result.appliedIds.size() == 1 && result.skippedIds.isEmpty());
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Q One").having == all && "renamed in all three");
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Q1").having.isEmpty() && "the old name is gone");
        if (orderIn("Q One") != before) {
            std::cerr << "a renamed playlist lost its tracks or their order\n";
            assert(false);
        }
        assert(logText().find("playlist-rename: renamed \"Q1\" to \"Q One\" in 3 libraries") != std::string::npos);
        {
            // Still where it was among its siblings in export.pdb.
            infrastructure::rekordbox::PdbRowWriter rows(pdbPath);
            std::map<std::string, uint32_t> order;
            for (const auto &n : rows.playlistTree()) {
                if (n.parentId == 0) {
                    order[n.name] = n.sortOrder;
                }
            }
            assert(order.count("Q One") && order["Q One"] < order["Fresh"] && "kept its place in the level");
        }

        // A name taken: every library has "Fresher".
        size_t mark = logText().size();
        result = save(rename("Fresh", "Fresher", all));
        assert(result.skippedIds.size() == 1 && "taken everywhere: a skip");
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Fresh").having == all && "left alone");
        for (const char *name : {"rekordbox", "OneLibrary", "Engine"}) {
            assert(logText().substr(mark).find("not renamed in \"Fresh\" there: " + std::string(name)
                                               + " has a playlist \"Fresher\" already")
                   != std::string::npos);
        }
        assert(logText().substr(mark).find("playlist-rename: \"Fresh\" renamed nowhere, skipped") != std::string::npos);

        // Engine's "AC/DC" names two playlists (case 6).
        mark = logText().size();
        result = save(rename("AC/DC", "Back", QStringList{QStringLiteral("engine")}));
        assert(result.skippedIds.size() == 1);
        assert(infrastructure::engine::enginePlaylistCountAtPath(pathToUtf8(library), "AC/DC") == 2 && "both still there");
        assert(logText().substr(mark).find("there: Engine has 2 playlists named \"AC/DC\"") != std::string::npos);

        // A smart playlist filed under "Q One" in Engine: Engine is left
        // alone, the other two are renamed.
        {
            sqlite3 *db = nullptr;
            assert(sqlite3_open(mdb.c_str(), &db) == SQLITE_OK);
            int tables = 0;
            sqlite3_exec(db, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'Smartlist';",
                         [](void *n, int, char **, char **) { ++*static_cast<int *>(n); return 0; }, &tables, nullptr);
            assert(tables == 1 && "the Engine library has a Smartlist table to plant in");
            assert(sqlite3_exec(db, "INSERT INTO Smartlist (listUuid, title, parentPlaylistPath, nextPlaylistPath, nextListUuid, rules) "
                                    "VALUES ('u-1', 'Smart', 'Q One;', '', '', '');",
                                nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(db);
        }
        mark = logText().size();
        result = save(rename("Q One", "Q Two", all));
        assert(result.appliedIds.size() == 1 && result.skippedIds.isEmpty());
        assert(logText().substr(mark).find("not renamed in \"Q One\" there: Engine has 1 smart playlists filed by its name")
               != std::string::npos);
        assert(logText().substr(mark).find("renamed \"Q One\" to \"Q Two\" in 2 libraries") != std::string::npos);
        assert(infrastructure::engine::enginePlaylistCountAtPath(pathToUtf8(library), "Q One") == 1 && "Engine left alone");
        assert((playlistWhereabouts(pioneerQ, libraryQ, "Q Two").having
                == QStringList{QStringLiteral("rekordbox"), QStringLiteral("onelibrary")}));
        {
            sqlite3 *db = nullptr;
            assert(sqlite3_open(mdb.c_str(), &db) == SQLITE_OK);
            assert(sqlite3_exec(db, "DELETE FROM Smartlist;", nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(db);
        }
        result = save(rename("Q One", "Q Two", QStringList{QStringLiteral("engine")}));
        assert(result.appliedIds.size() == 1 && playlistWhereabouts(pioneerQ, libraryQ, "Q Two").having == all);
        assert(std::get<2>(orderIn("Q Two")) == std::get<2>(before) && "Engine's entries in their order");

        // A folder: "Crate" holding "Inside" (with a02) in all three,
        // renamed to "Box": rekordbox's and OneLibrary's folder is no
        // playlist and keeps its name, said; Engine's "Crate" is a playlist
        // holding "Inside", renamed, the child's path following, its track
        // staying.
        {
            infrastructure::rekordbox::PdbRowWriter rows(pdbPath);
            const uint32_t crate = rows.createPlaylist(0, "Crate", true);
            const uint32_t inside = rows.createPlaylist(crate, "Inside", false);
            rows.appendPlaylistEntries(inside, {A02.pdbId});
            assert(rows.commit());
            infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(pioneer));
            w.createPlaylist("", "Crate", true);
            w.createPlaylist("Crate", "Inside", false);
            w.addToPlaylist("Crate/Inside", file(A02));
            infrastructure::engine::createEnginePlaylist(pathToUtf8(library), "Crate/Inside");
            infrastructure::engine::insertIntoEnginePlaylist(pathToUtf8(library), "Crate/Inside", engineId[A02.relative],
                                                             std::nullopt);
        }
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Crate/Inside").having == all);
        mark = logText().size();
        result = save(rename("Crate", "Box", all));
        assert(result.appliedIds.size() == 1 && result.skippedIds.isEmpty());
        const QStringList pioneerSide{QStringLiteral("rekordbox"), QStringLiteral("onelibrary")};
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Box/Inside").having == QStringList{QStringLiteral("engine")}
               && "Engine's child follows");
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Crate/Inside").having == pioneerSide && "the folders' stay");
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Crate").having == pioneerSide);
        assert(engineMembers(library, "Box/Inside") == std::vector<std::int64_t>{engineId[A02.relative]});
        assert(infrastructure::onelibrary::OneLibraryCueWriter(pathToUtf8(pioneer)).playlistContent("Crate/Inside").size() == 1);
        assert(logText().substr(mark).find("not renamed in \"Crate\" there: rekordbox's \"Crate\" is a folder")
               != std::string::npos);
        assert(result.notes.join(QLatin1Char(' ')).contains(
            QStringLiteral("\"Crate\" was not renamed in rekordbox and OneLibrary: there it is a folder")));

        // A name with a "/" fails, nothing written.
        {
            SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, {}, libraryQ);
            const SaveLoopResult bad = runSaveLoop({rename("Fresh", "A/B", all)}, ctx);
            assert(!bad.error.isEmpty() && "a name holding a slash fails");
            assert(playlistWhereabouts(pioneerQ, libraryQ, "Fresh").having == all);
        }
        {
            SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, {}, libraryQ);
            const SaveLoopResult bad = runSaveLoop({rename("Fresh", " Padded", all)}, ctx);
            assert(!bad.error.isEmpty() && "an untrimmed name fails");
            assert(playlistWhereabouts(pioneerQ, libraryQ, "Fresh").having == all);
            assert(playlistWhereabouts(pioneerQ, libraryQ, " Padded").having.isEmpty() && "written nowhere");
        }
    }
    std::cout << "case 10 (a playlist renamed in all three libraries; a folder keeps its name) OK\n";

    // 11. Which libraries have a row for a file, asked with the playlist
    //     in one lookup, as an add needs to know before it is staged: a01
    //     everywhere; b01 taken out of Engine (it is in no playlist since
    //     case 3), as a track rekordbox exported and Engine has not been
    //     synced to; a10 nowhere (deleted in case 4).
    {
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Q Two", file(A01)).withFile == all);
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Q Two").withFile.isEmpty() && "asked without a file: none");
        assert(infrastructure::engine::removeEngineTracks(pathToUtf8(library), {engineId[B01.relative]}) == 1);
        const auto b01 = playlistWhereabouts(pioneerQ, libraryQ, "Q Two", file(B01));
        assert((b01.withFile == QStringList{QStringLiteral("rekordbox"), QStringLiteral("onelibrary")})
               && "Engine has no row for it");
        assert(b01.having == all && "the playlist is in all three all the same");
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Q Two", file(A10)).withFile.isEmpty());
    }
    std::cout << "case 11 (the libraries with a row for a file, read with the playlist) OK\n";

    // 12. A playlist deleted with its tracks (Browse, "Delete playlist and
    //     its tracks"): "Doomed" holds c01, which no other playlist does,
    //     a01, which "Q Two" holds too, and b01, which Engine has no row
    //     for since case 11. The tracks' delete alone, with Doomed still
    //     there, takes nothing: every one of them is in a playlist. Staged
    //     behind the playlist's delete in one save, c01 and b01 go from
    //     every library that lists them, their files queued; a01 stays for
    //     Q Two.
    {
        const std::string pdbPath = pathToUtf8(pioneer / "rekordbox" / "export.pdb");
        const std::string manifestPath = pathToUtf8(stick / "Seabass" / "orphaned" / "pending-deletions.jsonl");
        const auto manifestText = [&]() {
            std::ifstream manifest(pathFromUtf8(manifestPath));
            std::stringstream lines;
            lines << manifest.rdbuf();
            return lines.str();
        };
        {
            infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(pioneer));
            assert(!w.isInAnyPlaylist(file(C01)) && !w.isInAnyPlaylist(file(B01)) && "in no playlist since case 3");
            assert(w.isInAnyPlaylist(file(A01)) && "a01 is in Q Two");
        }
        assert(save(std::make_shared<CreatePlaylistChange>(pioneerQ, libraryQ, "Doomed", all)).appliedIds.size() == 1);
        for (const auto &[f, title] : {std::pair{C01, "Tone C01"}, std::pair{A01, "Tone A01"}, std::pair{B01, "Tone B01"}}) {
            const auto added = save(std::make_shared<AddToPlaylistChange>(pioneerQ, libraryQ, "Doomed", file(f),
                                                                          QString::fromLatin1(title), all));
            assert(added.appliedIds.size() == 1 && added.skippedIds.isEmpty());
        }
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Doomed").having == all);
        assert((engineMembers(library, "Doomed") == std::vector<std::int64_t>{engineId[C01.relative], engineId[A01.relative]})
               && "Engine's Doomed is short of b01, which it has no row for");

        const auto rowsFor = [&](const File &f, bool inEngine) {
            std::vector<domain::Track> rows;
            for (const char *format : {"rekordbox", "onelibrary", "engine"}) {
                if (std::string(format) == "engine" && !inEngine) {
                    continue;
                }
                domain::Track t;
                t.format = format;
                t.filePath = file(f);
                rows.push_back(t);
            }
            return rows;
        };
        const auto tracksOfDoomed = [&]() {
            return std::shared_ptr<PendingChange>(DeleteTracksChange::forPlaylist(
                pioneerQ, libraryQ, "Doomed",
                {{file(C01), "Tone C01", "Tone Artist 01", rowsFor(C01, true)},
                 {file(A01), "Tone A01", "Tone Artist 01", rowsFor(A01, true)},
                 {file(B01), "Tone B01", "Nouvel Artiste", rowsFor(B01, false)}}));
        };
        assert(tracksOfDoomed()->id() == DeleteTracksChange::idForPlaylist("Doomed"));
        assert(tracksOfDoomed()->id() != DeleteTracksChange::idFor() && "beside Library Health's, never over it");
        assert(tracksOfDoomed()->owner() == QStringLiteral("addcue") && "Browse's editor stages it");

        // Alone, the playlist still there: nothing goes.
        {
            const auto result = save(tracksOfDoomed());
            assert(result.skippedIds.size() == 1 && "every track is still in a playlist");
            infrastructure::rekordbox::PdbRowWriter rows(pdbPath);
            assert(!rows.trackIdsWithFilePath("/Contents/" + std::string(C01.relative)).empty());
            assert(manifestText().find("c01.mp3") == std::string::npos);
        }

        // Behind the playlist's delete, in one save.
        {
            SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, {}, libraryQ);
            const SaveLoopResult result =
                runSaveLoop({std::make_shared<DeletePlaylistChange>(pioneerQ, libraryQ, "Doomed", all), tracksOfDoomed()}, ctx);
            assert(result.error.isEmpty());
            assert(result.appliedIds.size() == 2 && result.skippedIds.isEmpty());
        }
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Doomed").having.isEmpty() && "the playlist is gone from all three");
        {
            infrastructure::rekordbox::PdbRowWriter rows(pdbPath);
            assert(rows.trackIdsWithFilePath("/Contents/" + std::string(C01.relative)).empty() && "c01 out of export.pdb");
            assert(rows.trackIdsWithFilePath("/Contents/" + std::string(B01.relative)).empty() && "b01 out of export.pdb");
            assert(!rows.trackIdsWithFilePath("/Contents/" + std::string(A01.relative)).empty() && "a01 kept: Q Two holds it");
            infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(pioneer));
            for (const File &f : {C01, B01}) {
                bool gone = false;
                try {
                    w.addToPlaylist("Q Two", file(f));
                } catch (const infrastructure::onelibrary::OneLibraryRowMissing &) {
                    gone = true;
                }
                assert(gone && "out of OneLibrary");
            }
            assert(w.isInAnyPlaylist(file(A01)) && "a01 still in OneLibrary's Q Two");
            assert(w.playlistContentRowsWithoutPlaylist() == 0 && "no entry of Doomed left behind");
            auto db = djinterop::engine::load_database(pathToUtf8(library));
            assert(!db.track_by_id(engineId[C01.relative]).has_value() && "c01 out of Engine");
            assert(db.track_by_id(engineId[A01.relative]).has_value() && "a01 kept in Engine");
            assert(engineRowsNaming(library, engineId[C01.relative]) == 0 && "nothing of c01 left behind in Engine");
            const std::string queued = manifestText();
            assert(queued.find("c01.mp3") != std::string::npos && queued.find("b01.mp3") != std::string::npos
                   && "the files wait on the list");
            assert(queued.find("a01.mp3") == std::string::npos && "a01's file does not");
        }
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Q Two").having == all && "the other playlist is untouched");
    }
    std::cout << "case 12 (a playlist deleted with its tracks: only the ones no other playlist holds go) OK\n";

    // 13. An add asked for by a path that is a folder in rekordbox and in
    //     OneLibrary (a playlist only Engine has under that name, as Browse
    //     lists it): no entry is written against the folder; those two are
    //     left alone and the log says why, and Engine's playlist takes it.
    {
        {
            infrastructure::rekordbox::PdbRowWriter rows(pathToUtf8(pioneer / "rekordbox" / "export.pdb"));
            rows.createPlaylist(0, "Holder", true);
            assert(rows.commit());
            infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(pioneer));
            w.createPlaylist("", "Holder", true);
            auto db = djinterop::engine::load_database(pathToUtf8(library));
            db.create_root_playlist("Holder");
        }
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Holder").having == all && "the lookup counts the folders");
        const auto entriesOfHolder = [&]() {
            infrastructure::rekordbox::PdbRowWriter rows(pathToUtf8(pioneer / "rekordbox" / "export.pdb"));
            uint32_t holder = 0;
            for (const auto &n : rows.playlistTree()) {
                holder = n.name == "Holder" ? n.id : holder;
            }
            assert(holder != 0);
            return rows.playlistTrackIds(holder).size();
        };
        // The folder's own rows, by its path (the lookup takes any node).
        const auto oneLibraryRowsOfHolder = [&]() {
            return infrastructure::onelibrary::OneLibraryCueWriter(pathToUtf8(pioneer)).playlistContent("Holder").size();
        };
        const std::string logFile = infrastructure::backup::operationLogForStickRoot(
            infrastructure::backup::stickRootForCatalogPath(pathToUtf8(library)));
        const auto logText = [&]() {
            std::ifstream in(pathFromUtf8(logFile));
            std::stringstream all;
            all << in.rdbuf();
            return all.str();
        };
        const size_t logBefore = logText().size();
        const auto result = save(
            std::make_shared<AddToPlaylistChange>(pioneerQ, libraryQ, "Holder", file(A01), QStringLiteral("Tone A01"), all));
        assert(result.appliedIds.size() == 1 && "Engine took it");
        const size_t pdbEntries = entriesOfHolder();
        const size_t oneLibraryRows = oneLibraryRowsOfHolder();
        std::cerr << "case 13: export.pdb entries against the folder " << pdbEntries << ", OneLibrary rows "
                  << oneLibraryRows << "\n";
        assert(pdbEntries == 0 && "no export.pdb entry points at the folder");
        assert(oneLibraryRows == 0 && "no OneLibrary row points at the folder");
        assert(engineMembers(library, "Holder") == std::vector<std::int64_t>{engineId[A01.relative]}
               && "Engine's playlist of that name takes it");
        const std::string log = logText().substr(logBefore);
        assert(log.find("rekordbox's \"Holder\" is a folder") != std::string::npos);
        assert(log.find("OneLibrary's \"Holder\" is a folder") != std::string::npos);
        assert(result.notes.join(QLatin1Char(' '))
                   == QStringLiteral("\"Tone A01\" was not added to \"Holder\" in rekordbox and OneLibrary: there it "
                                     "is a folder, not a playlist.")
               && "the save's summary says so");
    }
    std::cout << "case 13 (an add by a path that is a folder in rekordbox and OneLibrary: nothing written against "
                 "the folder there, said in the log) OK\n";

    // 14. A delete and a rename asked for by a path that is a playlist in
    //     Engine and, in rekordbox and OneLibrary, a folder holding a
    //     playlist with a track (Browse lists the name, browsing Engine):
    //     the folders and what is in them stay, Engine's playlist goes or
    //     is renamed, and the log and the save's summary say what was left
    //     alone. An Engine playlist holding another is not deleted either:
    //     the one inside would go with it.
    {
        const std::string pdbPath = pathToUtf8(pioneer / "rekordbox" / "export.pdb");
        for (const std::string name : {"Mixed D", "Mixed R"}) {
            infrastructure::rekordbox::PdbRowWriter rows(pdbPath);
            const uint32_t folder = rows.createPlaylist(0, name, true);
            const uint32_t kept = rows.createPlaylist(folder, "Kept", false);
            rows.appendPlaylistEntries(kept, {A01.pdbId});
            assert(rows.commit());
            infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(pioneer));
            w.createPlaylist("", name, true);
            w.createPlaylist(name, "Kept", false);
            w.addToPlaylist(name + "/Kept", file(A01));
            infrastructure::engine::createEnginePlaylist(pathToUtf8(library), name);
        }
        infrastructure::engine::createEnginePlaylist(pathToUtf8(library), "Parent/Child");
        const QStringList pioneerSide{QStringLiteral("rekordbox"), QStringLiteral("onelibrary")};
        const auto keptIn = [&](const std::string &folder) {
            const auto where = playlistWhereabouts(pioneerQ, libraryQ, folder + "/Kept");
            if (where.having != pioneerSide) {
                return false;
            }
            return infrastructure::onelibrary::OneLibraryCueWriter(pathToUtf8(pioneer)).playlistContent(folder + "/Kept").size()
                   == 1;
        };
        assert(keptIn("Mixed D") && keptIn("Mixed R") && "precondition: each folder holds a playlist with a01");
        const auto mixed = playlistWhereabouts(pioneerQ, libraryQ, "Mixed D");
        assert(mixed.having == all && "the lookup finds a node at the path in each");

        const std::string logFile = infrastructure::backup::operationLogForStickRoot(
            infrastructure::backup::stickRootForCatalogPath(pathToUtf8(library)));
        const auto logText = [&]() {
            std::ifstream in(pathFromUtf8(logFile));
            std::stringstream text;
            text << in.rdbuf();
            return text.str();
        };
        size_t mark = logText().size();
        auto result = save(std::make_shared<DeletePlaylistChange>(pioneerQ, libraryQ, "Mixed D", all));
        std::cerr << "case 14: after the delete, rekordbox and OneLibrary have \"Mixed D/Kept\" in "
                  << playlistWhereabouts(pioneerQ, libraryQ, "Mixed D/Kept").having.join(QStringLiteral(", ")).toStdString()
                  << "\n";
        assert(keptIn("Mixed D") && "the folder's playlist and its track stay in rekordbox and OneLibrary");
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Mixed D").having == pioneerSide && "the folders stay");
        assert(infrastructure::engine::enginePlaylistCountAtPath(pathToUtf8(library), "Mixed D") == 0
               && "Engine's playlist is deleted");
        assert(result.appliedIds.size() == 1);
        std::string log = logText().substr(mark);
        assert(log.find("rekordbox's \"Mixed D\" is a folder") != std::string::npos);
        assert(log.find("OneLibrary's \"Mixed D\" is a folder") != std::string::npos);
        assert(result.notes.join(QLatin1Char(' ')).contains(
                   QStringLiteral("\"Mixed D\" was not deleted in rekordbox and OneLibrary"))
               && "the summary says so");

        mark = logText().size();
        result = save(std::make_shared<RenamePlaylistChange>(pioneerQ, libraryQ, "Mixed R", "Mixed S", all));
        assert(keptIn("Mixed R") && "the folder keeps its name and its playlist");
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Mixed S").having == QStringList{QStringLiteral("engine")}
               && "Engine's playlist is renamed, the folders are not");
        log = logText().substr(mark);
        assert(log.find("rekordbox's \"Mixed R\" is a folder") != std::string::npos);
        assert(result.notes.join(QLatin1Char(' ')).contains(
            QStringLiteral("\"Mixed R\" was not renamed in rekordbox and OneLibrary")));

        result = save(std::make_shared<DeletePlaylistChange>(pioneerQ, libraryQ, "Parent",
                                                             QStringList{QStringLiteral("engine")}));
        assert(result.skippedIds.size() == 1 && "Engine's playlist holding another is left alone: a skip");
        assert(infrastructure::engine::enginePlaylistCountAtPath(pathToUtf8(library), "Parent/Child") == 1
               && "the playlist inside stays");
        assert(result.notes.join(QLatin1Char(' ')).contains(QStringLiteral("\"Parent\" was not deleted in Engine")));
        const auto parent = playlistWhereabouts(pioneerQ, libraryQ, "Parent");
        assert(parent.holdingOthers == QStringList{QStringLiteral("engine")} && parent.folders.isEmpty()
               && "an Engine playlist holding another is no folder, and is said to hold others");
        // It takes tracks as any Engine playlist does (Engine has no folder
        // of its own kind; the reader lists its tracks), as Browse's add
        // always wrote it.
        result = save(std::make_shared<AddToPlaylistChange>(pioneerQ, libraryQ, "Parent", file(A01),
                                                            QStringLiteral("Tone A01"),
                                                            QStringList{QStringLiteral("engine")}));
        assert(result.appliedIds.size() == 1 && result.skippedIds.isEmpty() && result.notes.isEmpty());
        assert(engineMembers(library, "Parent") == std::vector<std::int64_t>{engineId[A01.relative]}
               && "Engine's playlist holding another takes the track");
    }
    std::cout << "case 14 (a delete and a rename by a path that is a folder in rekordbox and OneLibrary leave the "
                 "folder and what is in it alone, and say so; an Engine playlist holding another is not deleted) OK\n";

    // 15. A root playlist "Solo" without children in each library, and in
    //     Engine a second root playlist whose name holds a "/"
    //     ("Solo/Inner"; the rekordbox and OneLibrary writers refuse such
    //     a name): "Solo" holds no others (by the tree, not the spelling),
    //     so a delete takes it everywhere and leaves "Solo/Inner".
    {
        {
            infrastructure::rekordbox::PdbRowWriter rows(pathToUtf8(pioneer / "rekordbox" / "export.pdb"));
            rows.createPlaylist(0, "Solo", false);
            assert(rows.commit());
            infrastructure::onelibrary::OneLibraryCueWriter w(pathToUtf8(pioneer));
            w.createPlaylist("", "Solo", false);
            auto db = djinterop::engine::load_database(pathToUtf8(library));
            db.create_root_playlist("Solo");
            db.create_root_playlist("Solo/Inner");
        }
        const auto solo = playlistWhereabouts(pioneerQ, libraryQ, "Solo");
        assert(solo.having == all && solo.several.isEmpty());
        std::cerr << "case 15: \"Solo\" holds others in " << solo.holdingOthers.join(QStringLiteral(", ")).toStdString()
                  << "\n";
        assert(solo.holdingOthers.isEmpty() && "a root playlist named \"Solo/Inner\" is no child of \"Solo\"");
        const QStringList engineOnly{QStringLiteral("engine")};
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Solo/Inner").having == engineOnly);
        const auto result = save(std::make_shared<DeletePlaylistChange>(pioneerQ, libraryQ, "Solo", all));
        assert(result.appliedIds.size() == 1 && result.notes.isEmpty() && "deleted, nothing left alone");
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Solo").having.isEmpty() && "gone from all three");
        assert(playlistWhereabouts(pioneerQ, libraryQ, "Solo/Inner").having == engineOnly
               && "the other root playlist stays");
    }
    std::cout << "case 15 (a root playlist named with a \"/\" is no child of the one its name starts with) OK\n";

    // 16. A library on the stick that cannot be read is not one without
    // the playlist: it is named unreadable, and nothing throws, whether
    // the reader fails on it (a folder where the file should be) or even
    // asking whether the file is there fails (a link to itself).
    {
        const fs::path broken = testing::scratchRoot() / "seabass_playlist_edit_changes_test_unreadable";
        fs::remove_all(broken);
        fs::copy(stick, broken, fs::copy_options::recursive);
        const fs::path pdb = broken / "PIONEER" / "rekordbox" / "export.pdb";
        const fs::path oneLibrary = broken / "PIONEER" / "rekordbox" / "exportLibrary.db";
        const fs::path engineDb = broken / "Engine Library" / "Database2" / "m.db";
        assert(fs::exists(engineDb) && "precondition: Engine's database where the lookup reads it");
        const QString brokenPioneer = pathToQString(broken / "PIONEER");
        const QString brokenLibrary = pathToQString(broken / "Engine Library");
        assert(playlistWhereabouts(brokenPioneer, brokenLibrary, "Q1").unreadable.isEmpty() && "precondition");

        fs::remove(pdb);
        fs::create_directory(pdb);
        fs::remove(oneLibrary);
        fs::create_directory(oneLibrary);
        fs::remove(engineDb);
#if !defined(_WIN32)
        fs::create_symlink(engineDb.filename(), engineDb);  // a link to itself: asking about it fails
        std::error_code ec;
        (void)fs::exists(engineDb, ec);
        assert(ec && "precondition: whether m.db is there cannot even be asked");
#else
        // Windows makes no symlink without rights a test cannot count on:
        // a folder where m.db should be, as for the other two.
        fs::create_directory(engineDb);
#endif
        PlaylistWhereabouts where;
        try {
            where = playlistWhereabouts(brokenPioneer, brokenLibrary, "Q1", file(A01));
        } catch (const std::exception &e) {
            std::cerr << "case 16: the lookup threw: " << e.what() << "\n";
            assert(false && "the lookup throws nothing");
        }
        std::cerr << "case 16: unreadable " << where.unreadable.join(QStringLiteral(", ")).toStdString() << "\n";
        assert(where.unreadable == all && "each one that cannot be read is named");
        assert(where.onStick == all && "and counted as on the stick");
        assert(where.having.isEmpty() && where.folders.isEmpty() && where.holdingOthers.isEmpty()
               && where.withFile.isEmpty());
        fs::remove_all(broken);
    }
    std::cout << "case 16 (a library that cannot be read is said to be, not taken for one without it) OK\n";

    // 17. "Delete playlist and its tracks" names each file in every
    //     library on the stick: a row a library had none for is a stand-in
    //     (Entry::standIns). x97 and x98 are in Engine only, x99 nowhere.
    //     x97's rekordbox and OneLibrary rows are stand-ins: it goes. By
    //     one rule for all three, a real row (one the plan read) that is
    //     not found at Save fails the change before anything is removed,
    //     naming the file and the library: x98's real OneLibrary row, x98's
    //     real rekordbox row, x99's real Engine row. So does OneLibrary
    //     naming x97 in two spellings, stand-in or not. The change is
    //     applied directly, outside the save loop's rollback: a removal
    //     made before the failure would still be in the m.db it writes
    //     through. Library Health's entries carry only the rows of the
    //     libraries that have the file, none a stand-in: x98's Engine row
    //     alone, beside a02 in all three, goes from Engine.
    {
        std::map<std::string, std::int64_t> extra;
        {
            auto db = djinterop::engine::load_database(pathToUtf8(library));
            for (const char *name : {"x97.mp3", "x98.mp3"}) {
                djinterop::track_snapshot s;
                s.relative_path = std::string("../Contents/Extra/") + name;
                extra[name] = db.create_track(s).id();
            }
        }
        const auto entryFor = [&](const char *name, const QStringList &standIns) {
            DeleteTracksChange::Entry entry;
            entry.filePath = pathToUtf8(stick / "Contents" / "Extra" / name);
            entry.title = name;
            for (const QString &format : all) {
                domain::Track row;
                row.format = format.toStdString();
                row.filePath = entry.filePath;
                row.sourceId = format == QLatin1String("engine") ? std::to_string(extra[name]) : std::string();
                entry.rows.push_back(row);
                if (standIns.contains(format)) {
                    entry.standIns.push_back(format.toStdString());
                }
            }
            return entry;
        };
        const QStringList standIns{QStringLiteral("rekordbox"), QStringLiteral("onelibrary")};
        const auto engineHas = [&](const char *name) {
            return djinterop::engine::load_database(pathToUtf8(library)).track_by_id(extra[name]).has_value();
        };
        const auto failsBeforeRemoving = [&](const DeleteTracksChange::Entry &failing, const QString &words) {
            SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, {}, libraryQ);
            const auto change = DeleteTracksChange::forPlaylist(pioneerQ, libraryQ, "Gone", {entryFor("x97.mp3", standIns), failing});
            const ChangeOutcome outcome = change->apply(ctx);
            std::cerr << "case 17: " << outcome.error.toStdString() << "\n";
            assert(!outcome.ok && "the change fails");
            assert(outcome.error
                       == words.arg(QString::fromStdString(failing.filePath))
                              + QStringLiteral(" since Seabass read the stick, so no track was deleted. Check the stick "
                                               "again, then delete the tracks again.")
                   && "naming the library and the file, and what to do");
            FormatWriteSession *engineWrites = existingFormatWriteSession(ctx, "engine", libraryQ.toStdString());
            assert(engineWrites != nullptr && "the change wrote Engine through this session");
            auto db = djinterop::engine::load_database(engineWrites->writeRoot());
            assert(db.track_by_id(extra["x97.mp3"]).has_value() && db.track_by_id(extra["x98.mp3"]).has_value()
                   && "nothing removed, x97 neither");
        };
        failsBeforeRemoving(entryFor("x98.mp3", QStringList{QStringLiteral("rekordbox")}),
                            QStringLiteral("OneLibrary no longer lists \"%1\""));
        failsBeforeRemoving(entryFor("x98.mp3", QStringList{QStringLiteral("onelibrary")}),
                            QStringLiteral("rekordbox no longer lists \"%1\""));
        failsBeforeRemoving(entryFor("x99.mp3", standIns), QStringLiteral("Engine no longer lists \"%1\""));
        {
            // OneLibrary gains x97 in two spellings, neither the file's own.
            const std::string dbPath = infrastructure::onelibrary::OneLibraryCueWriter::dbPathFor(pathToUtf8(pioneer));
            const auto exec = [&](const std::string &sql) {
                infrastructure::onelibrary::SqlCipherLibrary lib;
                infrastructure::onelibrary::SqlCipherDb db(lib, dbPath, /*readOnly=*/false);
                db.exec(infrastructure::onelibrary::oneLibraryKeyPragma(dbPath));
                db.exec(sql);
            };
            exec("INSERT INTO content (content_id, title, path) VALUES (9701, 'x97', '/Contents/Extra/X97.mp3'), "
                 "(9702, 'x97', '/Contents/EXTRA/x97.mp3')");
            {
                SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, {}, libraryQ);
                const auto change = DeleteTracksChange::forPlaylist(pioneerQ, libraryQ, "Gone", {entryFor("x97.mp3", standIns)});
                const ChangeOutcome outcome = change->apply(ctx);
                std::cerr << "case 17: " << outcome.error.toStdString() << "\n";
                assert(!outcome.ok
                       && outcome.error
                              == QStringLiteral("OneLibrary lists two spellings of \"%1\" since Seabass read the stick, so "
                                                "no track was deleted. Check the stick again, then delete the tracks again.")
                                     .arg(QString::fromStdString(entryFor("x97.mp3", standIns).filePath))
                       && "two spellings are no stand-in's missing row");
            }
            exec("DELETE FROM content WHERE content_id IN (9701, 9702)");
        }
        const auto result = save(std::shared_ptr<PendingChange>(
            DeleteTracksChange::forPlaylist(pioneerQ, libraryQ, "Gone", {entryFor("x97.mp3", standIns)})));
        assert(result.appliedIds.size() == 1 && "an Engine-only track with stand-in rows goes");
        assert(!engineHas("x97.mp3") && engineHas("x98.mp3"));

        DeleteTracksChange::Entry a02{file(A02), "Tone A02", "Tone Artist 02", {}};
        for (const QString &format : all) {
            domain::Track row;
            row.format = format.toStdString();
            row.filePath = file(A02);
            a02.rows.push_back(row);
        }
        DeleteTracksChange::Entry x98{entryFor("x98.mp3", {}).filePath, "x98.mp3", "", {}};
        x98.rows.push_back(entryFor("x98.mp3", {}).rows.back());
        assert(x98.rows.front().format == "engine" && "precondition: x98's Engine row alone");
        const auto health = save(std::make_shared<DeleteTracksChange>(pioneerQ, libraryQ, std::vector{a02, x98}));
        assert(health.appliedIds.size() == 1 && "Library Health's delete of a file only Engine has is no failure");
        assert(!engineHas("x98.mp3") && engineRowsNaming(library, engineId[A02.relative]) > 0 && "x98 goes, a02 stays");
    }
    std::cout << "case 17 (a stand-in row a library has none for is nothing to remove; a real row missing in any library, "
                 "or two spellings in OneLibrary, fails the change before anything is removed; Library Health's rows "
                 "are all real) OK\n";

    // 18. A library that is not on the stick is not one that cannot be
    //     read: none is named unreadable, and the others are read as ever.
    {
        const fs::path absent = testing::scratchRoot() / "seabass_playlist_edit_changes_test_absent";
        const auto copyOfStick = [&]() {
            fs::remove_all(absent);
            fs::copy(stick, absent, fs::copy_options::recursive);
        };
        const QString absentPioneer = pathToQString(absent / "PIONEER");
        const QString absentLibrary = pathToQString(absent / "Engine Library");
        const QString engineOnly = QStringLiteral("engine");
        const QStringList pioneerOnly{QStringLiteral("rekordbox"), QStringLiteral("onelibrary")};
        const auto check = [&](const char *what, const QString &pioneerRoot, const QString &enginePath,
                               const QStringList &onStick) {
            const PlaylistWhereabouts where = playlistWhereabouts(pioneerRoot, enginePath, "Q Two");
            std::cerr << "case 18, " << what << ": on the stick " << where.onStick.join(QStringLiteral(", ")).toStdString()
                      << "; unreadable " << where.unreadable.join(QStringLiteral(", ")).toStdString() << "\n";
            assert(where.unreadable.isEmpty() && "an absent library is not unreadable");
            assert(where.onStick == onStick && where.having == onStick && "the others read as ever");
        };
        const auto removeDb = [&](const char *name) {
            for (const char *suffix : {"", "-wal", "-shm"}) {
                fs::remove(absent / "PIONEER" / "rekordbox" / (std::string(name) + suffix));
            }
        };

        copyOfStick();
        removeDb("export.pdb");
        check("a PIONEER folder without export.pdb", absentPioneer, absentLibrary,
              QStringList{QStringLiteral("onelibrary"), engineOnly});

        copyOfStick();
        removeDb("exportLibrary.db");
        check("rekordbox without exportLibrary.db", absentPioneer, absentLibrary,
              QStringList{QStringLiteral("rekordbox"), engineOnly});

        copyOfStick();
        fs::remove_all(absent / "PIONEER");
        check("only Engine, no PIONEER folder", absentPioneer, absentLibrary, QStringList{engineOnly});
        check("only Engine, no rekordbox path", QString(), absentLibrary, QStringList{engineOnly});

        copyOfStick();
        fs::remove_all(absent / "Engine Library");
        check("only rekordbox, no Engine Library folder", absentPioneer, absentLibrary, pioneerOnly);
        check("only rekordbox, no Engine path", absentPioneer, QString(), pioneerOnly);
        fs::remove_all(absent);
    }
    std::cout << "case 18 (a library not on the stick is not one that cannot be read) OK\n";

    fs::remove_all(stick);
    std::cout << "playlist_edit_changes_test: all cases passed\n";
    return 0;
}
