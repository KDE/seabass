// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Browse's playlist edits through the real save loop, on a stick holding
// the same playlists in all three libraries: rekordbox's own export.pdb
// (tests/fixtures/pdb_playlist_tree/3-folder.pdb: Q1 with a01..a05, folder
// F1 holding F1A with b01 and c01), the OneLibrary database rekordbox
// started from with the same playlists made in it, and an Engine library
// built to match. RemoveFromPlaylistChange takes a03 out of Q1 in all
// three, by path; DeletePlaylistChange deletes the folder F1 with F1A in
// all three, tracks kept.

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
#include <vector>

#include <djinterop/djinterop.hpp>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "gui/edit/changes/delete_tracks_change.hpp"
#include "gui/edit/changes/playlist_edit_changes.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/engine/engine_playlists.hpp"
#include "infrastructure/engine/engine_track_rows.hpp"
#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "scratch_path.hpp"

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
    assert(librariesWithPlaylist(pioneerQ, libraryQ, "Q1") == all);
    assert(librariesWithPlaylist(pioneerQ, libraryQ, "F1/F1A") == all);
    assert(librariesWithPlaylist(pioneerQ, libraryQ, "Nope").isEmpty());
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

    // 3. The folder F1 with F1A, from all three; the tracks stay.
    assert(librariesWithPlaylist(pioneerQ, libraryQ, "F1") == all);
    {
        const auto result = save(std::make_shared<DeletePlaylistChange>(pioneerQ, libraryQ, "F1", all));
        assert(result.appliedIds.size() == 1 && result.skippedIds.isEmpty());
    }
    assert(librariesWithPlaylist(pioneerQ, libraryQ, "F1").isEmpty());
    assert(librariesWithPlaylist(pioneerQ, libraryQ, "F1/F1A").isEmpty());
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
    assert(librariesWithPlaylist(pioneerQ, libraryQ, "Q1") == all && "the other playlist is untouched");
    std::cout << "case 3 (a folder deleted from all three libraries, tracks kept) OK\n";

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
        assert(librariesWithSeveralPlaylists(pioneerQ, libraryQ, "AC/DC") == QStringList{QStringLiteral("engine")});
        SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, {}, libraryQ);
        const SaveLoopResult result =
            runSaveLoop({std::make_shared<DeletePlaylistChange>(pioneerQ, libraryQ, "AC/DC", QStringList{QStringLiteral("engine")})}, ctx);
        assert(!result.error.isEmpty() && "an ambiguous path fails, it does not pick one");
        assert(infrastructure::engine::enginePlaylistCountAtPath(pathToUtf8(library), "AC/DC") == 2 && "both still there");
    }
    std::cout << "case 6 (an ambiguous playlist path is refused) OK\n";

    fs::remove_all(stick);
    std::cout << "playlist_edit_changes_test: all cases passed\n";
    return 0;
}
