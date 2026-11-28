// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Delete Tracks at Save finds a library's rows for a file the way the plan
// read them, on a copy of a real stick (tests/fixtures/anonymized_library).
// The readers accept a stored path that is not in normal form (export.pdb's
// "//Contents/...", "/Contents//...", "/Contents/./...", "/\Contents/...",
// see catalog_root_spelling_test; Engine's "../Contents//./..." or an
// absolute path) and fold letter case and accents when the plan groups rows
// by file. The plan here is read the way each page reads it: the stick's
// catalogs through readAllStickCatalogTracks, then Library Health's
// findTracksInNoPlaylist or Browse's planPlaylistTracksDeletion, with the
// stand-in rows Browse adds (PlaylistEditController's entriesOf). Each
// delete then goes through the real save loop.
//
// The rule at Save, one for the three libraries: every row of the file's
// key is the file, whatever its spelling. The file stays when any of them
// is in a playlist, and goes from all of them otherwise.
//
// Case spellings are spellings of database rows, never two files on disk
// that differ only in case: those cannot both exist on Windows or macOS.

#include <QString>
#include <QStringList>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <djinterop/djinterop.hpp>

#include "application/path_key.hpp"
#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/playlist_tracks_deletion.hpp"
#include "domain/tracks_in_no_playlist.hpp"
#include "gui/edit/changes/delete_tracks_change.hpp"
#include "gui/edit/changes/playlist_edit_changes.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/qt_path.hpp"
#include "gui/stick_catalogs.hpp"
#include "infrastructure/cleanup/pending_deletion_manifest.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/pdb_lookup.hpp"
#include "infrastructure/rekordbox/pdb_row_writer.hpp"
#include "engine_change_fixture.hpp"
#include "scratch_path.hpp"

#include <sqlite3.h>

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;
using infrastructure::rekordbox::PdbRowWriter;

namespace
{

const QStringList RekordboxOnly{QStringLiteral("rekordbox")};

[[noreturn]] void fail(const std::string &why)
{
    std::cerr << "delete_tracks_spelling_test: " << why << "\n";
    std::exit(1);
}

void require(bool ok, const std::string &why)
{
    if (!ok) {
        fail(why);
    }
}

std::string key(const std::string &path)
{
    return application::normalizedPathKey(path);
}

struct Stick
{
    testing::EngineChangeStick at;
    std::string pdbFile;
    std::string readersRoot;  // the stick root as the readers join it

    QString pioneer() const { return at.pioneerPath(); }
    QString engine() const { return at.enginePath(); }
    // The file `relative` (under the stick root) as a reader spells it.
    std::string file(const std::string &relative) const { return pathToUtf8(at.root / pathFromUtf8(relative)); }
};

// A track row in export.pdb whose file_path is `stored` exactly: made under
// `placeholder` (as long as `stored`, and a path addTrack takes), then
// pointed at `stored`. Returns its id.
uint32_t plantPdbRow(const Stick &stick, const std::string &placeholder, const std::string &stored)
{
    require(placeholder.size() == stored.size(), "a placeholder as long as " + stored);
    PdbRowWriter rows(stick.pdbFile);
    PdbRowWriter::NewTrack track;
    track.pathOnStick = placeholder;
    track.title = "Spelled " + stored;
    track.date = "2026-11-28";
    const uint32_t id = rows.addTrack(track);
    std::string refusal;
    const std::string filename = stored.substr(stored.find_last_of("/\\") + 1);
    require(rows.setTrackFilePath(id, filename, stored, &refusal), "planting " + stored + ": " + refusal);
    require(rows.commit(), "export.pdb written");
    return id;
}

uint32_t pdbPlaylist(const Stick &stick, const std::string &name, const std::vector<uint32_t> &tracks)
{
    PdbRowWriter rows(stick.pdbFile);
    uint32_t id = 0;
    for (const auto &node : rows.playlistTree()) {
        id = node.name == name && node.parentId == 0 ? node.id : id;
    }
    if (id == 0) {
        id = rows.createPlaylist(0, name, false);
    }
    rows.appendPlaylistEntries(id, tracks);
    require(rows.commit(), "export.pdb written");
    return id;
}

// The ids of export.pdb's rows whose file the readers spell with `fileKey`.
std::vector<uint32_t> pdbRowsOf(const Stick &stick, const std::string &fileKey)
{
    std::vector<uint32_t> ids;
    for (const auto &[id, stored] : PdbRowWriter(stick.pdbFile).trackFilePaths()) {
        if (!stored.empty() && key(infrastructure::rekordbox::trackFilePathOnStick(stick.readersRoot, stored)) == fileKey) {
            ids.push_back(id);
        }
    }
    return ids;
}

std::set<uint32_t> pdbIds(const Stick &stick)
{
    std::set<uint32_t> ids;
    for (const auto &[id, stored] : PdbRowWriter(stick.pdbFile).trackFilePaths()) {
        ids.insert(id);
    }
    return ids;
}

std::set<std::int64_t> engineIds(const Stick &stick)
{
    std::set<std::int64_t> ids;
    for (const auto &t : djinterop::engine::load_database(stick.at.engineUtf8()).tracks()) {
        ids.insert(t.id());
    }
    return ids;
}

void sql(const std::string &file, const std::string &statement)
{
    sqlite3 *db = nullptr;
    require(sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK, "open " + file);
    char *error = nullptr;
    const int rc = sqlite3_exec(db, statement.c_str(), nullptr, nullptr, &error);
    const std::string why = error ? error : "";
    sqlite3_free(error);
    sqlite3_close(db);
    require(rc == SQLITE_OK, statement + ": " + why);
}

// An Engine track whose Track.path is `stored` exactly, in `playlist` when
// one is named.
std::int64_t plantEngineRow(const Stick &stick, const std::string &stored, const std::string &playlist = {})
{
    std::int64_t id = 0;
    {
        auto db = djinterop::engine::load_database(stick.at.engineUtf8());
        djinterop::track_snapshot s;
        s.relative_path = "../Contents/Placeholder/" + std::to_string(db.tracks().size()) + ".mp3";
        auto t = db.create_track(s);
        id = t.id();
        if (!playlist.empty()) {
            auto list = db.root_playlist_by_name(playlist);
            (list ? *list : db.create_root_playlist(playlist)).add_track_back(t);
        }
    }
    std::string quoted;
    for (const char c : stored) {
        quoted += c == '\'' ? std::string("''") : std::string(1, c);
    }
    sql(stick.at.db, "UPDATE Track SET path = '" + quoted + "' WHERE id = " + std::to_string(id));
    return id;
}

void touchFile(const Stick &stick, const std::string &relative)
{
    const fs::path at = stick.at.root / pathFromUtf8(relative);
    fs::create_directories(at.parent_path());
    std::ofstream(at, std::ios::binary) << "not really audio";
}

std::vector<domain::PlaylistCatalog> readStick(const Stick &stick)
{
    LibraryCatalogCache::instance().invalidateEveryCatalogOn(pathToUtf8(stick.at.root));
    StickCatalogRead read = readAllStickCatalogTracks(pathToUtf8(stick.at.pioneer), application::CancellationToken::none());
    require(read.failed.empty(), "every library on the stick reads");
    std::vector<domain::PlaylistCatalog> catalogs;
    require(read.catalogs.rekordbox && read.catalogs.oneLibrary && read.catalogs.engine, "all three libraries are on the stick");
    catalogs.push_back({"rekordbox", std::move(*read.catalogs.rekordbox)});
    catalogs.push_back({"onelibrary", std::move(*read.catalogs.oneLibrary)});
    catalogs.push_back({"engine", std::move(*read.catalogs.engine)});
    return catalogs;
}

// Library Health's entries for the files `keys` among the tracks in no
// playlist, as deleteTracksInNoPlaylist builds them: the rows read, none a
// stand-in.
std::vector<DeleteTracksChange::Entry> healthEntries(const Stick &stick, const std::set<std::string> &keys)
{
    std::vector<DeleteTracksChange::Entry> entries;
    for (const auto &t : domain::findTracksInNoPlaylist(readStick(stick), key).tracks) {
        if (keys.count(key(t.filePath)) != 0) {
            entries.push_back({t.filePath, t.title, t.artist, t.rows});
        }
    }
    require(entries.size() == keys.size(), "Library Health lists each planted file in no playlist");
    return entries;
}

// Browse's entries for deleting `playlist` with its tracks: each going
// track's rows, and a stand-in for every library on the stick without one.
std::vector<DeleteTracksChange::Entry> browseEntries(const Stick &stick, const std::string &playlist, size_t going)
{
    const auto plan = domain::planPlaylistTracksDeletion(readStick(stick), key, playlist);
    require(plan.going.size() == going, "the plan for " + playlist + " lets " + std::to_string(going) + " tracks go, not "
                                            + std::to_string(plan.going.size()));
    std::vector<DeleteTracksChange::Entry> entries;
    for (const auto &track : plan.going) {
        DeleteTracksChange::Entry entry{track.filePath, track.title, track.artist, track.rows};
        for (const char *format : {"rekordbox", "onelibrary", "engine"}) {
            if (std::none_of(entry.rows.begin(), entry.rows.end(), [&](const domain::Track &row) { return row.format == format; })) {
                domain::Track row;
                row.format = format;
                row.filePath = track.filePath;
                entry.rows.push_back(row);
                entry.standIns.push_back(format);
            }
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

SaveLoopResult save(const Stick &stick, const std::vector<std::shared_ptr<PendingChange>> &changes)
{
    application::CancellationToken token;
    SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, stick.pioneer(), stick.engine());
    return runSaveLoop(changes, ctx);
}

std::shared_ptr<PendingChange> deletePlaylistWithTracks(const Stick &stick, const std::string &playlist,
                                                        std::vector<DeleteTracksChange::Entry> entries)
{
    return std::shared_ptr<PendingChange>(DeleteTracksChange::forPlaylist(stick.pioneer(), stick.engine(), playlist, std::move(entries)));
}

bool onPendingDeletions(const Stick &stick, const std::string &file)
{
    const infrastructure::cleanup::PendingDeletionManifest manifest(
        pathToUtf8(infrastructure::paths::stickPendingDeletions(stick.at.root)));
    const auto listed = manifest.list();
    return std::any_of(listed.begin(), listed.end(), [&](const auto &entry) { return application::samePath(entry.filePath, file); });
}

void requireLanded(const SaveLoopResult &result, const std::string &what)
{
    if (!result.error.isEmpty()) {
        fail(what + ": the save failed: " + result.error.toStdString());
    }
    require(result.skippedIds.isEmpty(), what + ": nothing skipped");
}

// The four spellings of a stored export.pdb path the reader takes for
// "/Contents/Spell/<letter><n>.mp3", each with a placeholder as long that
// addTrack takes, and the file it names, from the stick's root.
struct Spelling
{
    std::string placeholder;
    std::string stored;
    std::string relative;
};
std::vector<Spelling> spellingsOf(const std::string &letter)
{
    const auto named = [&](int n) { return letter + std::to_string(n) + ".mp3"; };
    return {{"/Contents/Spellx/" + named(1), "//Contents/Spell/" + named(1), "Contents/Spell/" + named(1)},
            {"/Contents/Spellx/" + named(2), "/Contents//Spell/" + named(2), "Contents/Spell/" + named(2)},
            {"/Contents/Spellxy/" + named(3), "/Contents/./Spell/" + named(3), "Contents/Spell/" + named(3)},
            {"/Contents/Spellx/" + named(4), "/\\Contents/Spell/" + named(4), "Contents/Spell/" + named(4)}};
}

std::shared_ptr<PendingChange> deletePlaylist(const Stick &stick, const std::string &playlist, const QStringList &libraries)
{
    return std::make_shared<DeletePlaylistChange>(stick.pioneer(), stick.engine(), playlist, libraries);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: delete_tracks_spelling_test <anonymized_library dir>\n";
        return 2;
    }
    testing::sandboxSeabassHome(testing::scratchRoot() / "delete_tracks_spelling_home");
    Stick stick{testing::makeEngineChangeStick(pathFromUtf8(argv[1]), "delete_tracks_spelling"), {}, {}};
    stick.pdbFile = pathToUtf8(stick.at.pioneer / "rekordbox" / "export.pdb");
    stick.readersRoot = infrastructure::paths::normalStickRootForCatalogPath(pathToUtf8(stick.at.pioneer));

    // 1. Library Health, tracks in no playlist: a row of each spelling.
    {
        std::set<std::string> keys;
        std::vector<std::string> files;
        for (const Spelling &s : spellingsOf("h")) {
            plantPdbRow(stick, s.placeholder, s.stored);
            touchFile(stick, s.relative);
            files.push_back(stick.file(s.relative));
            keys.insert(key(files.back()));
            require(PdbRowWriter(stick.pdbFile).trackIdsWithFilePath("/" + s.relative).empty(),
                    "precondition: " + s.stored + " is not found by its plain spelling");
        }
        const std::set<uint32_t> before = pdbIds(stick);
        const std::set<std::int64_t> engineBefore = engineIds(stick);
        const auto entries = healthEntries(stick, keys);
        for (const auto &e : entries) {
            require(e.rows.size() == 1 && e.rows.front().format == "rekordbox", "precondition: " + e.filePath + " in rekordbox only");
        }
        requireLanded(save(stick, {std::make_shared<DeleteTracksChange>(stick.pioneer(), stick.engine(), entries)}),
                      "Library Health's delete");
        for (const std::string &file : files) {
            require(pdbRowsOf(stick, key(file)).empty(), file + ": its row is gone");
            require(onPendingDeletions(stick, file), file + ": the file waits on the pending-deletions list");
        }
        const std::set<uint32_t> after = pdbIds(stick);
        require(after.size() + files.size() == before.size() && std::includes(before.begin(), before.end(), after.begin(), after.end()),
                "every other rekordbox track stays");
        require(engineIds(stick) == engineBefore, "Engine is untouched");
    }
    std::cout << "case 1 (Library Health deletes rekordbox rows stored in each spelling the reader takes) OK\n";

    // 2. Browse, a playlist deleted with its tracks: the same spellings, in
    //    a playlist of their own, every other library a stand-in.
    {
        std::vector<uint32_t> planted;
        std::vector<std::string> files;
        for (const Spelling &s : spellingsOf("p")) {
            planted.push_back(plantPdbRow(stick, s.placeholder, s.stored));
            touchFile(stick, s.relative);
            files.push_back(stick.file(s.relative));
        }
        pdbPlaylist(stick, "Spelled", planted);
        const std::set<uint32_t> before = pdbIds(stick);
        auto entries = browseEntries(stick, "Spelled", planted.size());
        for (const auto &e : entries) {
            require(e.standIns == std::vector<std::string>{"onelibrary", "engine"}, "precondition: " + e.filePath + " in rekordbox only");
        }
        requireLanded(save(stick, {deletePlaylist(stick, "Spelled", RekordboxOnly),
                                   deletePlaylistWithTracks(stick, "Spelled", std::move(entries))}),
                      "Browse's delete with tracks");
        for (const std::string &file : files) {
            require(pdbRowsOf(stick, key(file)).empty(), file + ": its row is gone");
            require(onPendingDeletions(stick, file), file + ": the file waits on the pending-deletions list");
        }
        const std::set<uint32_t> after = pdbIds(stick);
        require(after.size() + files.size() == before.size() && std::includes(before.begin(), before.end(), after.begin(), after.end()),
                "every other rekordbox track stays");
    }
    std::cout << "case 2 (Browse deletes a playlist's rekordbox rows stored in each spelling the reader takes) OK\n";

    // 3. rekordbox names one file in two letter cases. Read with one row,
    //    in the playlist being deleted; before Save rekordbox gains the
    //    other spelling, in another playlist. That row is the file too:
    //    the track stays, both rows with it.
    {
        const std::string file = stick.file("Contents/Case/c1.mp3");
        touchFile(stick, "Contents/Case/c1.mp3");
        const uint32_t first = plantPdbRow(stick, "/Contents/Casx/c1.mp3", "/Contents/Case/c1.mp3");
        pdbPlaylist(stick, "Case Doomed", {first});
        auto entries = browseEntries(stick, "Case Doomed", 1);
        const uint32_t second = plantPdbRow(stick, "/Contents/Casx/c1.mp3", "/Contents/CASE/c1.mp3");
        pdbPlaylist(stick, "Case Kept", {second});
        const auto result = save(stick, {deletePlaylist(stick, "Case Doomed", RekordboxOnly),
                                         deletePlaylistWithTracks(stick, "Case Doomed", std::move(entries))});
        require(result.error.isEmpty(), "the save lands: " + result.error.toStdString());
        require(result.skippedIds.contains(DeleteTracksChange::idForPlaylist("Case Doomed")), "the tracks' delete is a skip");
        const auto rows = pdbRowsOf(stick, key(file));
        require(rows.size() == 2, "both rekordbox rows stay, not " + std::to_string(rows.size()));
        require(!onPendingDeletions(stick, file), "the file is not queued");
    }
    // The same file in two spellings, both in no playlist: both go.
    {
        const std::string file = stick.file("Contents/Dup/d1.mp3");
        touchFile(stick, "Contents/Dup/d1.mp3");
        plantPdbRow(stick, "/Contents/Dux/d1.mp3", "/Contents/Dup/d1.mp3");
        plantPdbRow(stick, "/Contents/Dux/d1.mp3", "/Contents/DUP/d1.mp3");
        const auto entries = healthEntries(stick, {key(file)});
        require(entries.front().rows.size() == 2, "precondition: one entry with both rows");
        requireLanded(save(stick, {std::make_shared<DeleteTracksChange>(stick.pioneer(), stick.engine(), entries)}),
                      "Library Health's delete of two spellings");
        require(pdbRowsOf(stick, key(file)).empty(), "both spellings are gone");
        require(onPendingDeletions(stick, file), "the file is queued");
    }
    std::cout << "case 3 (every spelling of a file in rekordbox is the file: one in a playlist keeps it, none in one takes all) OK\n";

    // 4. A stand-in library that gains the file in another letter case,
    //    in a playlist, before Save keeps the track: Engine for a file
    //    only rekordbox listed, rekordbox for one only Engine listed.
    {
        const std::string file = stick.file("Contents/Stand/st1.mp3");
        touchFile(stick, "Contents/Stand/st1.mp3");
        const uint32_t row = plantPdbRow(stick, "/Contents/Stanx/st1.mp3", "/Contents/Stand/st1.mp3");
        pdbPlaylist(stick, "Stand Doomed", {row});
        auto entries = browseEntries(stick, "Stand Doomed", 1);
        require(entries.front().standIns == std::vector<std::string>{"onelibrary", "engine"}, "precondition: Engine is a stand-in");
        const std::int64_t engineRow = plantEngineRow(stick, "../Contents/STAND/st1.mp3", "Stand Kept");
        const auto result = save(stick, {deletePlaylist(stick, "Stand Doomed", RekordboxOnly),
                                         deletePlaylistWithTracks(stick, "Stand Doomed", std::move(entries))});
        require(result.error.isEmpty(), "the save lands: " + result.error.toStdString());
        require(result.skippedIds.contains(DeleteTracksChange::idForPlaylist("Stand Doomed")), "the tracks' delete is a skip");
        require(pdbRowsOf(stick, key(file)).size() == 1, "the rekordbox row stays");
        require(engineIds(stick).count(engineRow) == 1, "the Engine row stays");
        require(!onPendingDeletions(stick, file), "the file is not queued");
    }
    {
        const std::string file = stick.file("Contents/Eng/en1.mp3");
        touchFile(stick, "Contents/Eng/en1.mp3");
        const std::int64_t engineRow = plantEngineRow(stick, "../Contents/Eng/en1.mp3", "Eng Doomed");
        auto entries = browseEntries(stick, "Eng Doomed", 1);
        require(entries.front().standIns == std::vector<std::string>{"rekordbox", "onelibrary"}, "precondition: rekordbox is a stand-in");
        const uint32_t row = plantPdbRow(stick, "/Contents/Enx/en1.mp3", "/Contents/ENG/en1.mp3");
        pdbPlaylist(stick, "Eng Kept", {row});
        const auto result = save(stick, {deletePlaylist(stick, "Eng Doomed", QStringList{QStringLiteral("engine")}),
                                         deletePlaylistWithTracks(stick, "Eng Doomed", std::move(entries))});
        require(result.error.isEmpty(), "the save lands: " + result.error.toStdString());
        require(result.skippedIds.contains(DeleteTracksChange::idForPlaylist("Eng Doomed")), "the tracks' delete is a skip");
        require(pdbRowsOf(stick, key(file)).size() == 1, "the rekordbox row stays");
        require(engineIds(stick).count(engineRow) == 1, "the Engine row stays");
        require(!onPendingDeletions(stick, file), "the file is not queued");
    }
    std::cout << "case 4 (a stand-in library gaining the file in another case, in a playlist, keeps the track) OK\n";

    // 5. Engine rows stored out of normal form, as the Engine reader
    //    normalises them: a doubled separator and a ".", and an absolute
    //    path. Library Health deletes both.
    {
        const std::string odd = stick.file("Contents/Odd/e1.mp3");
        const std::string absolute = stick.file("Contents/Odd/e2.mp3");
        touchFile(stick, "Contents/Odd/e1.mp3");
        touchFile(stick, "Contents/Odd/e2.mp3");
        const std::int64_t e1 = plantEngineRow(stick, "../Contents//Odd/./e1.mp3");
        const std::int64_t e2 = plantEngineRow(stick, pathToGenericUtf8(pathFromUtf8(absolute)));
        const std::set<std::int64_t> before = engineIds(stick);
        const std::set<uint32_t> pdbBefore = pdbIds(stick);
        const auto entries = healthEntries(stick, {key(odd), key(absolute)});
        for (const auto &e : entries) {
            require(e.rows.size() == 1 && e.rows.front().format == "engine", "precondition: " + e.filePath + " in Engine only");
        }
        requireLanded(save(stick, {std::make_shared<DeleteTracksChange>(stick.pioneer(), stick.engine(), entries)}),
                      "Library Health's delete of Engine rows out of normal form");
        std::set<std::int64_t> expected = before;
        expected.erase(e1);
        expected.erase(e2);
        require(engineIds(stick) == expected, "both Engine rows are gone, every other stays");
        require(pdbIds(stick) == pdbBefore, "rekordbox is untouched");
        require(onPendingDeletions(stick, odd) && onPendingDeletions(stick, absolute), "both files are queued");
    }
    std::cout << "case 5 (Engine rows stored out of normal form are found and deleted) OK\n";

    std::cout << "delete_tracks_spelling_test: all cases passed\n";
    return 0;
}
