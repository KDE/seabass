// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The #73 hardware round of 2026-11-13 (stick TESTSHARK, rekordbox 7.2.18,
// Prime 4, Prime Go+), replayed on what the stick held at each step
// (tests/fixtures/testshark_t73, README.md there), through
// RekordboxExportSyncController and the session's save as the page drives
// them. Each case a ctest of its own, by name:
//
// fixture   The audio heads read as the stick's whole files did: TagLib's
//           sample rate and length, and the MP3 lead-in, as the stick's
//           metadata cache recorded them. And the chain checker
//           (engine_chain_check.hpp) finds what it is there to find: the
//           fixture's lists whole, a broken copy of them broken.
// proposal  Export 128 against the baseline at 40, Engine as before save 1:
//           exactly the 24 rows of expected/R3b-proposal-final.txt, all
//           ticked, nothing else on the page.
// save      Those 24 saved: Engine's playlists, tracks, rating, comment and
//           the new tracks' cues as the hardware round found them, and as
//           Seabass/1.0 wrote them on the stick (engine/m-after-R3c-
//           save1.db); every chain whole, the record at 128, the next
//           analysis empty. Undo: m.db as before (Engine's import counter
//           levelled, as every save with both catalogs leaves it) and the
//           record back at 40, the 24 rows again. Saved again: the same
//           m.db as the first save, row for row, but the write times.
// insync    The libraries match (#97). With the record at 128: nothing to
//           stage or save, Stage Selected not offered, nothing staged by
//           a press. Without a record: "Remember this export" records
//           export 128 through the session's save, backed up, with
//           nothing left staged, and says what it recorded; the page then
//           has the record, and Undo Last Save takes it away again.
// analysis  "Copy beat grids/waveforms from rekordbox to Engine when
//           syncing" (Preferences, application::SyncAnalysisPolicy) on the
//           same save, B01 made a row the player analysed (B04's analysis
//           copied onto it): both on, the four tracks the save writes (N1,
//           N2, N3 created, B01's rating) get rekordbox's grid and waveform,
//           the created rows completed over their files' exact length,
//           B01's own analysis kept, the ledger written, the summary's note
//           saying what landed, every other row as it was; Undo takes all
//           of it back. B01's grid locked on the player: left alone and
//           counted. Grids only: B01's grid, the new rows left for the
//           player. Both off, explicitly or never set: the save the "save"
//           case makes, the same m.db and the same files on the stick.
// replay    What the players wrote after the saves (engine/m-after-R3d.db,
//           the Prime 4's analysis; engine/m-after-R1b.db, the Go+) against
//           export 128 and the record at 128: nothing on the page, no cue
//           row in any section; the cues the players kept are the samples
//           Seabass wrote.
//
// argv[1]: tests/fixtures/testshark_t73, argv[2]: the case.

#include <QCoreApplication>
#include <QSignalSpy>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <djinterop/djinterop.hpp>
#include <djinterop/engine/v3/track_data_blob.hpp>
#include <sqlite3.h>

#include "engine_chain_check.hpp"
#include "engine_change_fixture.hpp"
#include "application/sync_analysis_policy.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/engine_library_creation_task.hpp"
#include "gui/library_catalog_cache.hpp"
#include "gui/rekordbox_export_sync_controller.hpp"
#include "gui/sync_controller.hpp"
#include "infrastructure/engine/engine_import_state.hpp"
#include "infrastructure/engine/libdjinterop_engine_library_creator.hpp"
#include "gui/rekordbox_export_sync_list_model.hpp"
#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/exact_sample_count.hpp"
#include "infrastructure/audio/taglib_metadata_probe.hpp"
#endif
#include "infrastructure/local/beat_grid_ledger_file.hpp"
#include "infrastructure/local/cached_sample_rates.hpp"
#include "infrastructure/local/kept_engine_analysis_file.hpp"
#include "infrastructure/local/rekordbox_baseline_file.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "nothing_else_running.hpp"
#include "scratch_path.hpp"

using seabass::pathToGenericUtf8;
using seabass::pathFromUtf8;
using seabass::pathToUtf8;
using seabass::gui::EditSessionRegistry;
using seabass::gui::LibraryEditSession;
using seabass::gui::RekordboxExportSyncController;
using seabass::gui::RekordboxExportSyncListModel;
using Section = RekordboxExportSyncListModel::Section;
using Row = RekordboxExportSyncListModel::Row;
namespace fs = std::filesystem;
namespace testing = seabass::testing;
namespace d = seabass::domain;

namespace
{

fs::path g_fixture;

// A stick under the scratch root: the fixture's PIONEER (export 128) and
// Contents, `engineDb` as Engine's m.db, `baseline` as Seabass's record.
// An empty `baseline` leaves the stick without one.
testing::EngineChangeStick makeStick(const std::string &name, const std::string &engineDb,
                                     const std::string &baseline)
{
    testing::EngineChangeStick stick;
    stick.root = testing::scratchRoot() / name;
    fs::remove_all(stick.root);
    fs::create_directories(stick.root);
    stick.pioneer = stick.root / "PIONEER";
    stick.engine = stick.root / "Engine Library";
    stick.db = pathToUtf8(stick.engine / "Database2" / "m.db");
    std::error_code ec;
    testing::copyPioneerFixture(g_fixture / "stick" / "PIONEER", stick.pioneer, ec);
    assert(!ec && "the fixture's PIONEER copies");
    fs::copy(g_fixture / "stick" / "Contents", stick.root / "Contents", fs::copy_options::recursive, ec);
    assert(!ec && "the fixture's audio copies");
    fs::create_directories(stick.engine / "Database2");
    fs::copy_file(g_fixture / "engine" / engineDb, stick.engine / "Database2" / "m.db", ec);
    assert(!ec && "the Engine state copies");
    if (baseline.empty()) {
        return stick;  // no record yet: the first visit
    }
    const fs::path record = seabass::infrastructure::paths::stickRekordboxBaseline(stick.root);
    fs::create_directories(record.parent_path());
    fs::copy_file(g_fixture / "baseline" / baseline, record, ec);
    assert(!ec && "the record copies");
    return stick;
}

fs::path recordOf(const testing::EngineChangeStick &stick)
{
    return seabass::infrastructure::paths::stickRekordboxBaseline(stick.root);
}

// An Engine state of the fixture as a library of its own, for reading
// with libdjinterop and SQLite without touching the fixture.
fs::path engineCopy(const std::string &engineDb, const std::string &name)
{
    const fs::path library = testing::scratchRoot() / name / "Engine Library";
    fs::remove_all(library.parent_path());
    fs::create_directories(library / "Database2");
    fs::copy_file(g_fixture / "engine" / engineDb, library / "Database2" / "m.db");
    return library;
}

void waitUntilIdle(RekordboxExportSyncController &controller)
{
    QSignalSpy busy(&controller, &RekordboxExportSyncController::busyChanged);
    for (int i = 0; i < 3000 && controller.busy(); ++i) {
        busy.wait(100);
    }
    assert(!controller.busy());
}

void analyze(RekordboxExportSyncController &controller, const testing::EngineChangeStick &stick)
{
    controller.analyze(QStringLiteral("TESTSHARK"), stick.pioneerPath(), stick.enginePath());
    waitUntilIdle(controller);
    if (!controller.errorMessage().isEmpty()) {
        std::cerr << "analysis: " << controller.errorMessage().toStdString() << "\n";
    }
    assert(controller.errorMessage().isEmpty());
    assert(controller.analyzed());
}

LibraryEditSession *sessionOf(const testing::EngineChangeStick &stick)
{
    auto *registry = EditSessionRegistry::instance();
    LibraryEditSession *session = registry->sessionFor(registry->libraryIdForPath(stick.pioneerPath()));
    assert(session);
    return session;
}

QVariantMap waitForSave(LibraryEditSession *session, RekordboxExportSyncController &controller, bool undo)
{
    QSignalSpy finished(session, &LibraryEditSession::saveFinished);
    if (undo) {
        controller.undoLastOperation();
    } else {
        session->save();
    }
    for (int i = 0; i < 1200 && finished.isEmpty(); ++i) {
        finished.wait(100);
    }
    assert(!finished.isEmpty() && "the save finished");
    // The page is analysed again after every save of its session.
    waitUntilIdle(controller);
    const QVariantMap summary = finished.takeFirst().at(0).toMap();
    std::cout << "  " << (undo ? "undo" : "save") << ": " << summary.value("written").toInt() << " of "
              << summary.value("total").toInt() << " " << summary.value("unit").toString().toStdString() << " "
              << summary.value("verb").toString().toStdString() << "; error \""
              << summary.value("error").toString().toStdString() << "\", warning \""
              << summary.value("warning").toString().toStdString() << "\"\n";
    assert(summary.value("error").toString().isEmpty());
    assert(summary.value("warning").toString().isEmpty());
    return summary;
}

// One row of the proposal as what it writes, no wording: the edit's kind
// and its paths, values and anchor.
std::string describe(const Row &row)
{
    if (!row.edit) {
        return "no edit: " + row.kind.toStdString() + " " + row.header.key;
    }
    struct Describe
    {
        std::string operator()(const d::PlaylistCreate &e) const
        {
            return std::string("create ") + (e.folder ? "folder" : "playlist") + " " + e.path;
        }
        std::string operator()(const d::PlaylistRename &e) const { return "rename " + e.fromPath + " to " + e.toPath; }
        std::string operator()(const d::PlaylistDelete &e) const
        {
            return std::string("delete ") + (e.folder ? "folder" : "playlist") + " " + e.path + " ("
                + std::to_string(e.engineMembers) + ")";
        }
        std::string operator()(const d::TrackToAdd &e) const { return "add " + e.pathKey; }
        std::string operator()(const d::TrackToRemove &e) const
        {
            return "remove " + e.pathKey + " (" + std::to_string(e.playlistCount) + ")";
        }
        std::string operator()(const d::MembershipEdit &e) const
        {
            if (e.kind == d::MembershipEdit::Kind::Remove) {
                return "take out of " + e.playlistPath + ": " + e.pathKey;
            }
            return "put into " + e.playlistPath + ": " + e.pathKey + " after "
                + (e.afterPathKey.empty() ? std::string("(first)") : e.afterPathKey);
        }
        std::string operator()(const d::MetadataEdit &e) const
        {
            const std::string to = e.direction == d::MetadataEdit::Direction::ToEngine ? "Engine" : "rekordbox";
            if (e.field == d::MetadataEdit::Field::Rating) {
                return "rating to " + to + " " + e.pathKey + ": "
                    + (e.rating ? std::to_string(*e.rating) : std::string("none"));
            }
            return "comment to " + to + " " + e.pathKey + ": \"" + e.comment + "\"";
        }
        std::string operator()(const d::CueEdit &e) const
        {
            return std::string("cues to ")
                + (e.plan.direction == d::SyncPlan::Direction::ToB ? "Engine " : "rekordbox ") + e.pathKey;
        }
    };
    return std::visit(Describe{}, *row.edit);
}

// What the page lists, every row described, ticked or not.
std::multiset<std::string> listed(RekordboxExportSyncController &controller)
{
    std::multiset<std::string> out;
    for (const Row &row : controller.rows()->rows()) {
        out.insert(std::string(row.included ? "[x] " : "[ ] ") + describe(row));
    }
    return out;
}

const std::string Base = "contents/seabass test/t73 base/";
const std::string New = "contents/seabass test/t73/";
const std::string Unknown = "contents/unknownartist/unknownalbum/";

// expected/R3b-proposal-final.txt's 24 rows, every one ticked.
std::multiset<std::string> expectedProposal()
{
    const std::string b01 = Base + "t73-b01-click-44k-102bpm.flac";
    const std::string b02 = Base + "t73-b02-click-44k-104bpm.flac";
    const std::string b03 = Base + "t73-b03-click-44k-106bpm.flac";
    const std::string b04 = Base + "t73-b04-click-44k-108bpm.mp3";
    const std::string b08 = Base + "t73-b08-click-44k-116bpm.flac";
    const std::string b12 = Base + "t73-b12-click-44k-124bpm.flac";
    const std::string n1 = New + "t73-n1-click-48k-120bpm.flac";
    const std::string n2 = Unknown + "t73-n2-click-48k-125bpm.aiff";
    const std::string n3 = New + "t73-n3-click-44k-128bpm.flac";
    std::multiset<std::string> out{
        // Playlists to create, rename, delete (5)
        "create folder T73 Folder",
        "create playlist T73 Renamed",
        "create playlist T73 Folder/T73 New",
        "delete playlist T73 Delete Me (2)",
        "delete playlist T73 Rename Me (4)",
        // Tracks to add to Engine (3)
        "add " + n1,
        "add " + n3,
        "add " + n2,
        // Tracks to remove from Engine (4)
        "remove " + Base + "t73-b05-click-44k-110bpm.flac (1)",
        "remove " + Base + "t73-b09-click-44k-118bpm.mp3 (1)",
        "remove " + Base + "t73-b10-click-44k-120bpm.flac (1)",
        "remove " + Unknown + "t73-b06-click-48k-112bpm.aiff (1)",
        // Playlist membership (10)
        "take out of T73 Order: " + b08,
        "put into T73 Order: " + b08 + " after " + b12,
        "put into T73 Folder/T73 New: " + n1 + " after (first)",
        "put into T73 Folder/T73 New: " + n2 + " after " + n1,
        "put into T73 Folder/T73 New: " + n3 + " after " + n2,
        "put into T73 Renamed: " + b01 + " after (first)",
        "put into T73 Renamed: " + b02 + " after " + b01,
        "put into T73 Renamed: " + b03 + " after " + b02,
        "put into T73 Renamed: " + b04 + " after " + b03,
        "put into T73 Renamed: " + b12 + " after " + b04,
        // Ratings and comments to Engine (2)
        "rating to Engine " + b01 + ": 4",
        "comment to Engine " + b01 + ": \"T73 comment\"",
    };
    std::multiset<std::string> ticked;
    for (const auto &row : out) {
        ticked.insert("[x] " + row);
    }
    return ticked;
}

bool sameList(const std::multiset<std::string> &want, const std::multiset<std::string> &got)
{
    if (want == got) {
        return true;
    }
    for (const auto &w : want) {
        if (!got.count(w)) {
            std::cerr << "  missing: " << w << "\n";
        }
    }
    for (const auto &g : got) {
        if (!want.count(g)) {
            std::cerr << "  not expected: " << g << "\n";
        }
    }
    return false;
}

// "T73-B07-click-44k-114bpm.flac" -> "B07", "T73-N1-click-..." -> "N1".
std::string shortName(const std::string &filename)
{
    const auto first = filename.find('-');
    const auto second = first == std::string::npos ? first : filename.find('-', first + 1);
    return second == std::string::npos ? filename : filename.substr(first + 1, second - first - 1);
}

// What a player shows of an Engine library, by the tracks' short names:
// the playlist tree in order with each list's members in chain order, and
// per track its rating, comment, sample rate and cues as sample offsets.
// Ids, timestamps and uuids are left out, so two saves of the same
// proposal compare equal. Asserts the chains walk (checkEngineChains).
std::map<std::string, std::string> engineFacts(const fs::path &library)
{
    const std::string db = pathToUtf8(library / "Database2" / "m.db");
    const testing::EngineChainReport chains = testing::checkEngineChains(db);
    if (!chains.whole()) {
        std::cerr << db << ":\n" << chains.text();
    }
    assert(chains.whole() && "every chain walks");

    std::map<std::int64_t, std::string> nameOf;
    for (const auto &r : testing::sqlRows(db, "SELECT id, filename FROM Track;")) {
        nameOf[std::stoll(r[0])] = shortName(r[1]);
    }
    std::map<std::int64_t, std::string> titleOf;
    for (const auto &r : testing::sqlRows(db, "SELECT id, title FROM Playlist;")) {
        titleOf[std::stoll(r[0])] = r[1];
    }
    std::map<std::string, std::string> facts;
    // The tree, depth first from the roots.
    std::vector<std::pair<std::int64_t, std::string>> pending{{0, ""}};
    while (!pending.empty()) {
        const auto [parent, prefix] = pending.back();
        pending.pop_back();
        const auto it = chains.children.find(parent);
        if (it == chains.children.end()) {
            continue;
        }
        std::string names;
        for (const std::int64_t child : it->second) {
            names += (names.empty() ? "" : ", ") + titleOf.at(child);
            const std::string path = prefix + titleOf.at(child);
            std::string members;
            if (const auto e = chains.entryTracks.find(child); e != chains.entryTracks.end()) {
                for (const std::int64_t track : e->second) {
                    members += (members.empty() ? "" : ", ") + nameOf.at(track);
                }
            }
            facts["list " + path] = members;
            pending.emplace_back(child, path + "/");
        }
        facts["children of " + (prefix.empty() ? std::string("(root)") : prefix)] = names;
    }

    auto engine = djinterop::engine::load_database(pathToUtf8(library));
    for (const auto &r : testing::sqlRows(db, "SELECT id, rating, comment FROM Track;")) {
        const auto track = engine.track_by_id(std::stoll(r[0]));
        assert(track);
        std::ostringstream out;
        out << std::setprecision(12);
        out << "rating " << r[1] << ", comment \"" << (r[2] == "<null>" ? "" : r[2]) << "\", rate "
            << track->sample_rate().value_or(0.0) << ", main cue ";
        if (const auto main = track->main_cue()) {
            out << *main;
        } else {
            out << "none";
        }
        const auto hot = track->hot_cues();
        for (std::size_t pad = 0; pad < hot.size(); ++pad) {
            if (hot[pad]) {
                out << ", pad " << pad + 1 << " " << hot[pad]->sample_offset << " \"" << hot[pad]->label << "\"";
            }
        }
        const auto loops = track->loops();
        for (std::size_t pad = 0; pad < loops.size(); ++pad) {
            if (loops[pad]) {
                out << ", loop " << pad + 1 << " " << loops[pad]->start_sample_offset << "-"
                    << loops[pad]->end_sample_offset;
            }
        }
        facts["track " + nameOf.at(std::stoll(r[0]))] = out.str();
    }
    return facts;
}

bool sameFacts(const std::map<std::string, std::string> &want, const std::map<std::string, std::string> &got,
               const char *what)
{
    if (want == got) {
        return true;
    }
    std::cerr << "  " << what << " differs:\n";
    std::set<std::string> keys;
    for (const auto &[k, v] : want) {
        keys.insert(k);
    }
    for (const auto &[k, v] : got) {
        keys.insert(k);
    }
    for (const auto &k : keys) {
        const auto w = want.find(k);
        const auto g = got.find(k);
        if (w == want.end() || g == got.end() || w->second != g->second) {
            std::cerr << "    " << k << "\n      want: " << (w == want.end() ? "(none)" : w->second)
                      << "\n      got:  " << (g == got.end() ? "(none)" : g->second) << "\n";
        }
    }
    return false;
}

void printFacts(const std::map<std::string, std::string> &facts)
{
    for (const auto &[k, v] : facts) {
        std::cout << "    " << k << ": " << v << "\n";
    }
}

// The facts the hardware round checked by hand after save 1 (session
// README, R3c and R2): the playlists, the tracks left, B01's rating and
// comment, the new tracks' cues at their own rate.
void assertSavedAsOnHardware(const std::map<std::string, std::string> &facts)
{
    const auto at = [&facts](const std::string &key) {
        const auto it = facts.find(key);
        if (it == facts.end()) {
            std::cerr << "  no " << key << "\n";
        }
        assert(it != facts.end());
        return it->second;
    };
    const auto starts = [](const std::string &text, const std::string &head) {
        if (text.rfind(head, 0) != 0) {
            std::cerr << "  \"" << text << "\" does not start with \"" << head << "\"\n";
        }
        return text.rfind(head, 0) == 0;
    };
    assert(at("children of (root)") == "T73 Order, T73 Folder, T73 Renamed");
    assert(at("children of T73 Folder/") == "T73 New");
    assert(at("list T73 Order") == "B07, B11, B12, B08");
    assert(at("list T73 Renamed") == "B01, B02, B03, B04, B12");
    assert(at("list T73 Folder/T73 New") == "N1, N2, N3");
    std::set<std::string> tracks;
    for (const auto &[k, v] : facts) {
        if (k.rfind("track ", 0) == 0) {
            tracks.insert(k.substr(6));
        }
    }
    assert((tracks == std::set<std::string>{"B01", "B02", "B03", "B04", "B07", "B08", "B11", "B12", "N1", "N2",
                                             "N3"}));
    assert(starts(at("track B01"), "rating 80, comment \"T73 comment\""));
    // N1: rekordbox's hot cues A 30 s and B 60 s, as samples at the
    // file's 48 kHz (a save leaves the row's rate for the player to
    // analyse; the samples are what it reads).
    assert(at("track N1").find(", pad 1 1440000 ") != std::string::npos);
    assert(at("track N1").find(", pad 2 2880000 ") != std::string::npos);
    // N2: rekordbox's memory cue at 28.8 s on pad 1, as Engine DJ's import
    // puts it; the automatic one at 0:00 not carried.
    assert(at("track N2").find(", pad 1 1382400 ") != std::string::npos);
    assert(at("track N2").find(", pad 2 ") == std::string::npos);
}

// A table's rows in id order without the columns that hold the time of
// the write (dateAdded, lastEditTime).
std::vector<std::vector<std::string>> rowsButClock(const std::string &db, const std::string &table)
{
    std::string columns;
    for (const auto &c : testing::sqlRows(db, "PRAGMA table_info(" + table + ");")) {
        if (c[1] != "dateAdded" && c[1] != "lastEditTime") {
            columns += (columns.empty() ? "\"" : ", \"") + c[1] + "\"";
        }
    }
    assert(!columns.empty());
    return testing::sqlRows(db, "SELECT " + columns + " FROM " + table + " ORDER BY id;");
}

std::string fileBytes(const fs::path &file)
{
    return testing::fileBytes(file);
}

// --- fixture -----------------------------------------------------------

void testFixture()
{
    // What the stick's Seabass/caches/metadata.jsonl recorded for the
    // whole files after save 1 (N3 was never probed there: 44.1 kHz,
    // 240 s by how it was made).
    struct Expected
    {
        const char *file;
        int rate;
        double seconds;
    };
    const Expected expected[] = {
        {"Seabass Test/T73 Base/T73-B01-click-44k-102bpm.flac", 44100, 180.0},
        {"Seabass Test/T73 Base/T73-B02-click-44k-104bpm.flac", 44100, 180.0},
        {"Seabass Test/T73 Base/T73-B03-click-44k-106bpm.flac", 44100, 180.0},
        {"Seabass Test/T73 Base/T73-B04-click-44k-108bpm.mp3", 44100, 180.036},
        {"Seabass Test/T73 Base/T73-B07-click-44k-114bpm.flac", 44100, 180.0},
        {"Seabass Test/T73 Base/T73-B08-click-44k-116bpm.flac", 44100, 180.0},
        {"Seabass Test/T73 Base/T73-B12-click-44k-124bpm.flac", 44100, 180.0},
        {"Seabass Test/T73/T73-N1-click-48k-120bpm.flac", 48000, 240.0},
        {"Seabass Test/T73/T73-N3-click-44k-128bpm.flac", 44100, 240.0},
        {"UnknownArtist/UnknownAlbum/T73-B11-click-48k-122bpm.aiff", 48000, 180.0},
        {"UnknownArtist/UnknownAlbum/T73-N2-click-48k-125bpm.aiff", 48000, 240.0},
    };
    std::size_t onDisk = 0;
    for (auto it = fs::recursive_directory_iterator(g_fixture / "stick" / "Contents");
         it != fs::recursive_directory_iterator(); ++it) {
        onDisk += it->is_regular_file() ? 1 : 0;
    }
    assert(onDisk == std::size(expected) && "the eleven files on the stick at export 128, no others");

#ifdef SEABASS_HAVE_TAGLIB
    seabass::infrastructure::audio::TagLibMetadataProbe probe;
#else
    // Never a pass without the probe the save itself needs: an add to
    // Engine reads the file's sample rate through TagLib.
    std::cerr << "built without TagLib: the fixture's audio cannot be checked, and the save cannot add tracks\n";
    std::exit(1);
    seabass::application::NullTrackMetadataProbe probe;
#endif
    for (const auto &e : expected) {
        const fs::path file = g_fixture / "stick" / "Contents" / pathFromUtf8(e.file);
        const auto read = probe.read(pathToUtf8(file));
        if (!read) {
            std::cerr << "  TagLib cannot read " << e.file << "\n";
        }
        assert(read);
        std::cout << "  " << e.file << ": " << read->sampleRate << " Hz, " << read->durationSeconds << " s\n";
        assert(read->sampleRate == e.rate);
        assert(std::abs(read->durationSeconds - e.seconds) < 0.0005);
        assert(!read->durationIsEstimated);
    }
    // B04's lead-in, as the cache had it for the whole file:
    // "4 1 C 1 576 1008 0 0 44100 1152 1 1".
    {
        const auto read =
            probe.read(pathToUtf8(g_fixture / "stick" / "Contents" / "Seabass Test" / "T73 Base"
                                  / "T73-B04-click-44k-108bpm.mp3"));
        assert(read && read->mp3LeadInRead && read->mp3LeadIn);
        const d::Mp3LeadIn &l = *read->mp3LeadIn;
        assert(l.infoFrame && l.tag == d::Mp3LeadIn::Tag::Lavc && l.lameCrcValid);
        assert(l.encoderDelay == 576 && l.encoderPadding == 1008);
        assert(!l.infoFrameModeMatchesAudio && !l.vbri);
        assert(l.sampleRate == 44100 && l.samplesPerFrame == 1152);
        assert(l.tagFrameFirst && l.tagFrameHasCounts);
    }
    std::cout << "fixture: the audio heads read as the whole files did OK\n";

    // The chain checker: every Engine state of the fixture walks whole.
    for (const char *state : {"m-before-R3.db", "m-after-R3c-save1.db", "m-after-R3d.db", "m-after-R1b.db"}) {
        const fs::path library = engineCopy(state, std::string("chains_") + state);
        const auto report = testing::checkEngineChains(pathToUtf8(library / "Database2" / "m.db"));
        if (!report.whole()) {
            std::cerr << state << ":\n" << report.text();
        }
        assert(report.whole());
        assert(report.children.at(0).size() == 3 && "three playlists at the top, in every state");
    }
    // And finds each way a chain breaks, on copies of save 1's m.db.
    // Engine's delete trigger relinks round a deleted entry, so the breaks
    // are made by UPDATE, as a writer that got it wrong would.
    const auto broken = [](const char *name, const std::string &sql, const std::string &finding) {
        const fs::path library = engineCopy("m-after-R3c-save1.db", std::string("broken_") + name);
        const std::string db = pathToUtf8(library / "Database2" / "m.db");
        assert(testing::sqlExec(db, sql));
        const auto report = testing::checkEngineChains(db);
        std::cout << "  " << name << ":\n" << report.text();
        assert(!report.whole());
        bool found = false;
        for (const auto &p : report.problems) {
            found = found || p.find(finding) != std::string::npos;
        }
        assert(found && "the checker names the break");
    };
    // "T73 Renamed" (list 5) cut after its head: two heads, two tails.
    broken("cut", "UPDATE PlaylistEntity SET nextEntityId = 0 WHERE listId = 5 AND id NOT IN "
                  "(SELECT nextEntityId FROM PlaylistEntity WHERE listId = 5);",
           "list 5: 2 head(s) and 2 tail(s)");
    // Its tail pointed back at its head.
    broken("loop", "UPDATE PlaylistEntity SET nextEntityId = (SELECT id FROM PlaylistEntity WHERE listId = 5 AND "
                   "id NOT IN (SELECT nextEntityId FROM PlaylistEntity WHERE listId = 5)) WHERE listId = 5 AND "
                   "nextEntityId = 0;",
           "0 head(s) and 0 tail(s)");
    // Its tail pointed into "T73 Order" (list 2).
    broken("into another list", "UPDATE PlaylistEntity SET nextEntityId = (SELECT min(id) FROM PlaylistEntity WHERE "
                                "listId = 2) WHERE listId = 5 AND nextEntityId = 0;",
           "which is not in this chain");
    // An entry of a track that does not exist.
    broken("no track", "UPDATE PlaylistEntity SET trackId = 999 WHERE id = (SELECT min(id) FROM PlaylistEntity);",
           "names track 999, which does not exist");
    // The last root playlist pointing at one that does not exist.
    broken("siblings", "UPDATE Playlist SET nextListId = 999 WHERE parentListId = 0 AND nextListId = 0;",
           "points at 999, which is not in this chain");
    std::cout << "fixture: the chain checker finds the fixture whole and every planted break OK\n";
}

// --- proposal ----------------------------------------------------------

void assertGoldenProposal(RekordboxExportSyncController &controller)
{
    assert(controller.hasBaseline());
    assert(controller.baselineSequence() == 40);
    assert(controller.currentSequence() == 128);
    assert(controller.introText()
           == QStringLiteral("Compared with how this stick looked when Seabass last saved it, export 40."));
    const auto got = listed(controller);
    assert(sameList(expectedProposal(), got));
    assert(controller.checkedCount() == 24);
    assert(controller.conflictCount() == 0);
    const QVariantMap counts = controller.sectionCounts();
    const std::map<QString, int> want{{"conflicts", 0},        {"playlists", 5},          {"tracksToAdd", 3},
                                      {"tracksToRemove", 4},   {"membership", 10},        {"metadataToEngine", 2},
                                      {"cuesToEngine", 0},     {"restoresToRekordbox", 0}, {"engineOwnKept", 0},
                                      {"engineOnlyPlaylists", 0}, {"notAdded", 0}};
    for (auto it = counts.begin(); it != counts.end(); ++it) {
        const auto w = want.find(it.key());
        if (w == want.end() || w->second != it.value().toInt()) {
            std::cerr << "  section " << it.key().toStdString() << ": " << it.value().toInt() << "\n";
        }
        assert(w != want.end() && w->second == it.value().toInt());
    }
    assert(counts.size() == static_cast<int>(want.size()));
}

void testProposal()
{
    const auto stick = makeStick("t73_proposal", "m-before-R3.db", "rekordbox-baseline-export40.tsv.gz");
    const std::string before = testing::fileBytes(stick.db);
    RekordboxExportSyncController controller;
    analyze(controller, stick);
    assertGoldenProposal(controller);
    assert(testing::fileBytes(stick.db) == before && "the analysis wrote nothing");
    std::cout << "proposal: export 128 against the record at 40 is R3b-proposal-final.txt's 24 rows OK\n";
}

// --- save, undo, save --------------------------------------------------

void testSaveUndoSave()
{
    const auto stick = makeStick("t73_save", "m-before-R3.db", "rekordbox-baseline-export40.tsv.gz");
    const testing::DatabaseDump before = testing::dumpDatabase(stick.db);
    const std::string record40 = fileBytes(recordOf(stick));
    const std::string informationSql = "SELECT id, uuid, schemaVersionMajor, schemaVersionMinor, schemaVersionPatch, "
                                       "currentPlayedIndiciator, lastRekordBoxLibraryImportReadCounter FROM "
                                       "Information;";
    const auto informationBefore = testing::sqlRows(stick.db, informationSql);
    assert(informationBefore.size() == 1 && informationBefore[0].back() == "40");

    RekordboxExportSyncController controller;
    analyze(controller, stick);
    assertGoldenProposal(controller);
    controller.stageSelected();
    assert(controller.errorMessage().isEmpty());
    assert(controller.stagedCount() == 24);
    LibraryEditSession *session = sessionOf(stick);
    for (const auto &line : session->pendingDescriptions()) {
        std::cout << "    staged: " << line.toStdString() << "\n";
    }

    // Save 1. The hardware round's page said "16 changes made".
    const QVariantMap first = waitForSave(session, controller, false);
    assert(first.value("written").toInt() == 16 && first.value("total").toInt() == 16);
    assert(first.value("unit").toString() == QStringLiteral("changes"));
    assert(controller.proposalEmpty() && controller.rows()->rowCount() == 0 && "nothing left after the save");
    assert(controller.baselineSequence() == 128);
    const auto saved1 = engineFacts(stick.engine);
    printFacts(saved1);
    assertSavedAsOnHardware(saved1);
    const auto hardware = engineFacts(engineCopy("m-after-R3c-save1.db", "t73_hardware_save1"));
    assert(sameFacts(hardware, saved1, "save 1 against Seabass/1.0's save 1 on the stick"));
    const testing::DatabaseDump dump1 = testing::dumpDatabase(stick.db);
    const auto playlist1 = rowsButClock(stick.db, "Playlist");
    const auto track1 = rowsButClock(stick.db, "Track");
    assert(testing::sqlRows(stick.db, "SELECT lastRekordBoxLibraryImportReadCounter FROM Information;")[0][0]
           == "128");
    std::cout << "save: the 24 rows saved as the hardware round found them, every chain whole OK\n";

    // Undo: m.db as before but Engine's import counter, which the undo
    // (a save with both catalogs) leaves level with export 128; the
    // record back at 40, byte for byte; the 24 rows again.
    assert(controller.canUndo());
    waitForSave(session, controller, true);
    {
        testing::DatabaseDump after = testing::dumpDatabase(stick.db);
        testing::DatabaseDump beforeButInformation = before;
        beforeButInformation.erase("Information");
        after.erase("Information");
        const auto diffs = testing::differences(beforeButInformation, after);
        testing::printDifferences(diffs);
        assert(diffs.empty());
        const auto information = testing::sqlRows(stick.db, informationSql);
        assert(information.size() == 1);
        for (std::size_t c = 0; c + 1 < information[0].size(); ++c) {
            assert(information[0][c] == informationBefore[0][c]);
        }
        assert(information[0].back() == "128");
        const auto chains = testing::checkEngineChains(stick.db);
        assert(chains.whole());
    }
    assert(fileBytes(recordOf(stick)) == record40);
    assertGoldenProposal(controller);
    std::cout << "undo: m.db and the record as before save 1, the 24 rows again OK\n";

    // Save 2: the same Engine as save 1.
    controller.stageSelected();
    assert(controller.stagedCount() == 24);
    waitForSave(session, controller, false);
    assert(controller.proposalEmpty() && controller.rows()->rowCount() == 0);
    const auto saved2 = engineFacts(stick.engine);
    assert(sameFacts(saved1, saved2, "save 2 against save 1"));
    // Row for row the same m.db as save 1 but the clock: the same ids,
    // entries, tracks, cues and import counter. Playlist.lastEditTime and
    // Track.dateAdded and lastEditTime are the time of the write (whole
    // seconds in Track, so two saves on a loaded machine differ there).
    {
        testing::DatabaseDump dump2 = testing::dumpDatabase(stick.db);
        testing::DatabaseDump dump1ButClocked = dump1;
        for (const char *table : {"Playlist", "Track"}) {
            dump1ButClocked.erase(table);
            dump2.erase(table);
        }
        const auto diffs = testing::differences(dump1ButClocked, dump2);
        testing::printDifferences(diffs);
        assert(diffs.empty());
        assert(rowsButClock(stick.db, "Playlist") == playlist1);
        assert(rowsButClock(stick.db, "Track") == track1);
    }
    std::cout << "save again: the same m.db as save 1 but its write times, nothing left to propose OK\n";
}

// --- in sync (#97) -----------------------------------------------------

void testInSync()
{
    // The record at 128 and Engine after save 2: nothing to do.
    {
        const auto stick = makeStick("t73_insync_recorded", "m-after-R3d.db", "rekordbox-baseline-export128.tsv.gz");
        const std::string record = fileBytes(recordOf(stick));
        RekordboxExportSyncController controller;
        analyze(controller, stick);
        assert(controller.hasBaseline() && controller.proposalEmpty());
        assert(!controller.canStage() && "nothing ticked and nothing to decline: nothing to stage");
        LibraryEditSession *session = sessionOf(stick);
        controller.stageSelected();
        assert(session->pendingCount() == 0 && "a press stages nothing");
        controller.rememberExport();
        assert(session->pendingCount() == 0 && !session->writing() && "the record is there already");
        assert(fileBytes(recordOf(stick)) == record);
        std::cout << "insync: with the record, nothing to stage and nothing saved OK\n";
    }

    // No record yet: the first visit after Create Engine Library.
    const auto stick = makeStick("t73_insync_first", "m-after-R3d.db", "");
    assert(!fs::exists(recordOf(stick)));
    RekordboxExportSyncController controller;
    analyze(controller, stick);
    for (const Row &row : controller.rows()->rows()) {
        std::cerr << "  first visit lists: " << row.kind.toStdString() << " " << describe(row) << "\n";
    }
    assert(!controller.hasBaseline());
    assert(controller.proposalEmpty() && "the libraries match");
    LibraryEditSession *session = sessionOf(stick);
    assert(!session->canUndo());

    QSignalSpy recorded(&controller, &RekordboxExportSyncController::recordSaved);
    QSignalSpy finished(session, &LibraryEditSession::saveFinished);

    // A remember whose save fails leaves nothing staged: no Save offering
    // the record, the failure said, and the button there for a second try.
    // A file where the save's backup folder has to go: the save cannot
    // make its backup, so the record is never applied. A file, not a
    // permission: permissions do not stop root (CI) nor work alike on
    // every platform.
    const fs::path blocker = stick.root / "Seabass" / "backups";
    fs::create_directories(blocker.parent_path());
    assert(!fs::exists(blocker) && "precondition: no backup folder yet");
    {
        std::ofstream(blocker) << "not a folder";
    }
    controller.rememberExport();
    for (int i = 0; i < 1200 && finished.isEmpty(); ++i) {
        finished.wait(100);
    }
    assert(!finished.isEmpty() && "the refused save finished");
    waitUntilIdle(controller);
    fs::remove(blocker);
    {
        const QVariantMap refused = finished.takeFirst().at(0).toMap();
        std::cout << "  refused remember: error \"" << refused.value("error").toString().toStdString() << "\"\n";
        assert(!refused.value("error").toString().isEmpty() && "precondition: the save failed");
    }
    std::cout << "  page says: " << controller.errorMessage().toStdString() << "\n";
    assert(session->pendingCount() == 0 && !session->dirty() && "nothing left staged after a remember that failed");
    assert(controller.stagedCount() == 0);
    assert(controller.errorMessage().startsWith(QStringLiteral("This export was not remembered: ")));
    assert(recorded.isEmpty());
    assert(!fs::exists(recordOf(stick)));
    assert(!controller.hasBaseline() && controller.proposalEmpty() && "Remember this export is offered again");
    std::cout << "insync: a remember whose save fails leaves nothing staged and says why OK\n";

    controller.rememberExport();
    assert(session->writing() && "saved at once, nothing left for the user to stage and save");
    for (int i = 0; i < 1200 && finished.isEmpty(); ++i) {
        finished.wait(100);
    }
    assert(!finished.isEmpty() && "the save finished");
    waitUntilIdle(controller);
    const QVariantMap summary = finished.takeFirst().at(0).toMap();
    std::cout << "  remember: error \"" << summary.value("error").toString().toStdString() << "\", warning \""
              << summary.value("warning").toString().toStdString() << "\"\n";
    assert(summary.value("error").toString().isEmpty());
    assert(summary.value("warning").toString().isEmpty());
    assert(session->pendingCount() == 0 && !session->dirty());
    assert(controller.stagedCount() == 0);

    std::string error;
    const auto baseline = seabass::infrastructure::local::readRekordboxBaseline(stick.root, &error);
    assert(error.empty() && baseline && "the record is on the stick");
    assert(baseline->pdbSequence == 128);
    assert(!baseline->tracks.empty());
    assert(recorded.size() == 1 && "the page is told what was recorded");
    const QString said = recorded.takeFirst().at(0).toString();
    std::cout << "  said: " << said.toStdString() << "\n";
    assert(said.startsWith(QStringLiteral("Remembered this export: ")));
    assert(said.contains(QString::number(baseline->tracks.size()) + QStringLiteral(" tracks")));
    assert(!said.contains(QStringLiteral("record")) && !said.contains(QStringLiteral("0 ")));
    assert(controller.hasBaseline() && controller.proposalEmpty() && controller.baselineSequence() == 128);
    assert(session->canUndo() && "backed up first: Undo Last Save takes it away");
    std::cout << "insync: without a record, Remember this export records export 128 at once OK\n";

    controller.undoLastOperation();
    for (int i = 0; i < 1200 && finished.isEmpty(); ++i) {
        finished.wait(100);
    }
    assert(!finished.isEmpty() && "the undo finished");
    waitUntilIdle(controller);
    assert(finished.takeFirst().at(0).toMap().value("error").toString().isEmpty());
    assert(!fs::exists(recordOf(stick)) && "the undo takes the first record away");
    assert(!controller.hasBaseline() && controller.proposalEmpty());
    assert(recorded.isEmpty() && "an undo is not a record saved");
    std::cout << "insync: Undo Last Save takes the first record away again OK\n";

    // Not in sync, every row unticked: Stage Selected records what was
    // left (declined), and the page is told so, the save counting nothing.
    {
        const auto other = makeStick("t73_insync_declined", "m-before-R3.db", "rekordbox-baseline-export40.tsv.gz");
        RekordboxExportSyncController page;
        analyze(page, other);
        assert(page.canStage() && page.checkedCount() > 0);
        for (const char *section : {"playlists", "tracksToAdd", "tracksToRemove", "membership", "metadataToEngine",
                                    "cuesToEngine", "restoresToRekordbox"}) {
            page.setSectionIncluded(QString::fromLatin1(section), false);
        }
        assert(page.checkedCount() == 0);
        assert(page.canStage() && "nothing ticked, but the rows left unticked are recorded as declined");
        page.stageSelected();
        LibraryEditSession *otherSession = sessionOf(other);
        assert(otherSession->pendingCount() == 1 && "the record alone");
        QSignalSpy saidDeclined(&page, &RekordboxExportSyncController::recordSaved);
        const QVariantMap declinedSummary = waitForSave(otherSession, page, false);
        assert(declinedSummary.value("total").toInt() == 0);
        assert(saidDeclined.size() == 1);
        assert(saidDeclined.takeFirst().at(0).toString() == QStringLiteral("Recorded your choices for the next sync."));
        std::cout << "insync: a stage of nothing ticked records the declines and says so OK\n";
    }

    // An empty answer the record settles: B01's comment changed on both
    // sides since the record (BothChanged), answered with Engine's side,
    // which writes nothing. Staged as the record alone, saved, and the
    // conflict is not asked again.
    {
        const auto both = makeStick("t73_insync_both_changed", "m-after-R3d.db", "rekordbox-baseline-export128.tsv.gz");
        assert(testing::sqlExec(both.db, "UPDATE Track SET comment = 'Engine comment' WHERE filename LIKE "
                                         "'T73-B01-%';"));
        std::string error;
        auto record = seabass::infrastructure::local::readRekordboxBaseline(both.root, &error);
        assert(error.empty() && record);
        int planted = 0;
        for (auto &track : record->tracks) {
            if (track.comment == "T73 comment") {
                track.comment = "Older comment";
                ++planted;
            }
        }
        assert(planted == 1 && "precondition: the record knew B01's comment");
        assert(seabass::infrastructure::local::writeRekordboxBaseline(
            both.root, *record, [](const std::string &) {}, &error));
        RekordboxExportSyncController page;
        analyze(page, both);
        int row = -1;
        std::string key;
        for (std::size_t i = 0; i < page.rows()->rows().size(); ++i) {
            const Row &r = page.rows()->rows()[i];
            std::cerr << "  both changed lists: " << r.kind.toStdString() << " " << describe(r) << "\n";
            if (r.section == Section::Conflicts && r.conflict
                && r.header.reason == d::EngineUpdateReason::BothChanged && r.conflict->engineChoice.empty()) {
                row = static_cast<int>(i);
                key = r.header.key;
            }
        }
        assert(row >= 0 && "precondition: the comment is a BothChanged conflict whose Engine side writes nothing");
        assert(page.hasBaseline() && !page.canStage());
        page.resolveConflict(row, false);
        assert(page.checkedCount() == 0 && page.canStage() && "an empty answer the record settles is staged");
        page.stageSelected();
        LibraryEditSession *bothSession = sessionOf(both);
        assert(bothSession->pendingCount() == 1 && "the record alone");
        QSignalSpy said(&page, &RekordboxExportSyncController::recordSaved);
        const QVariantMap summary = waitForSave(bothSession, page, false);
        assert(summary.value("total").toInt() == 0);
        assert(said.size() == 1);
        for (const Row &r : page.rows()->rows()) {
            assert(!(r.section == Section::Conflicts && r.header.key == key) && "the answered conflict is not asked again");
        }
        std::cout << "insync: an empty answer to a comment both sides changed is recorded and not asked again OK\n";
    }
}

// --- replay ------------------------------------------------------------

void testReplay()
{
    for (const char *state : {"m-after-R3d.db", "m-after-R1b.db"}) {
        const auto stick =
            makeStick(std::string("t73_replay_") + state, state, "rekordbox-baseline-export128.tsv.gz");
        RekordboxExportSyncController controller;
        analyze(controller, stick);
        assert(controller.hasBaseline() && controller.baselineSequence() == 128);
        for (const Row &row : controller.rows()->rows()) {
            std::cerr << "  " << state << " lists: " << row.kind.toStdString() << " " << describe(row) << "\n";
        }
        assert(controller.rows()->rowCount() == 0 && "no row in any section, cue rows included");
        assert(controller.proposalEmpty());
        assert(!controller.onlyCues());

        // The players kept what Seabass wrote: N1's pads at 30 and 60 s,
        // N2's at 28.8 s, B11's at 30 s, at 48 kHz; and analysed them.
        const auto facts = engineFacts(stick.engine);
        printFacts(facts);
        assertSavedAsOnHardware(facts);
        assert(facts.at("track B11").find(", pad 1 1440000 ") != std::string::npos);
        for (const char *track : {"track N1", "track N2", "track B11"}) {
            assert(facts.at(track).find("rate 48000") != std::string::npos);
        }
        for (const char *analysed : {"N1", "N2", "B11"}) {
            const auto rows = testing::sqlRows(stick.db, std::string("SELECT isAnalyzed FROM Track WHERE filename LIKE "
                                                                     "'T73-")
                                                             + analysed + "-%';");
            assert(rows.size() == 1 && rows[0][0] == "1" && "the Prime 4 analysed it");
        }
        std::cout << "replay " << state << ": nothing to propose after the players, no cue rows OK\n";
    }
}

// --- rekordbox's analysis with the sync ------------------------------------

using seabass::application::SyncAnalysisPolicy;

// Track id by the file's short name ("B01", "N1").
std::int64_t trackIdOf(const testing::EngineChangeStick &stick, const std::string &name)
{
    return testing::sqlScalar(stick.db, "SELECT id FROM Track WHERE filename LIKE 'T73-" + name + "-%';");
}

// Every row's analysis, the columns a copy may write, by track id.
std::map<std::int64_t, std::vector<std::string>> analysisRows(const std::string &db)
{
    std::map<std::int64_t, std::vector<std::string>> out;
    for (auto row : testing::sqlRows(db, "SELECT t.id, t.isAnalyzed, t.isBeatGridLocked, t.bpmAnalyzed, t.key, "
                                         "p.trackData, p.overviewWaveFormData, p.beatData FROM Track t LEFT JOIN "
                                         "PerformanceData p ON p.trackId = t.id ORDER BY t.id;")) {
        const std::int64_t id = std::stoll(row.front());
        row.erase(row.begin());
        out[id] = std::move(row);
    }
    return out;
}

enum Column { IsAnalyzed = 0, Locked = 1, BpmAnalyzed = 2, Key = 3, TrackData = 4, Overview = 5, BeatData = 6 };

// The player analysed B01: B04's analysis (a Prime 4's own, from S4) on
// B01's row, the columns a player writes. `locked`: and its grid locked.
void pretendPlayerAnalysedB01(const testing::EngineChangeStick &stick, bool locked)
{
    const std::string b01 = std::to_string(trackIdOf(stick, "B01"));
    const std::string b04 = std::to_string(trackIdOf(stick, "B04"));
    const bool done = testing::sqlExec(
        stick.db, "UPDATE PerformanceData SET trackData = (SELECT trackData FROM PerformanceData WHERE trackId = " + b04
                      + "), overviewWaveFormData = (SELECT overviewWaveFormData FROM PerformanceData WHERE trackId = "
                      + b04 + "), beatData = (SELECT beatData FROM PerformanceData WHERE trackId = " + b04
                      + ") WHERE trackId = " + b01 + "; UPDATE Track SET isAnalyzed = 1, isBeatGridLocked = "
                      + (locked ? "1" : "0") + " WHERE id = " + b01 + ";");
    assert(done);
    (void)done;
    assert(analysisRows(stick.db).at(std::stoll(b01))[IsAnalyzed] == "1");
}

// The files under `dir`, by path from it with '/' on every platform (the
// keys are compared with "Database2/m.db" and the like), with their bytes;
// none when it is not there.
std::map<std::string, std::string> filesUnder(const fs::path &dir)
{
    std::map<std::string, std::string> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return out;
    }
    for (const auto &entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            out[pathToGenericUtf8(fs::relative(entry.path(), dir))] = fileBytes(entry.path());
        }
    }
    return out;
}

fs::path overviewDirOf(const testing::EngineChangeStick &stick)
{
    return stick.engine / "Database2" / "OverviewData";
}

fs::path keptDirOf(const testing::EngineChangeStick &stick)
{
    return seabass::infrastructure::local::keptEngineAnalysisFile(stick.root, "x").parent_path();
}

std::vector<std::byte> blobOf(const std::string &db, const std::string &sql)
{
    sqlite3 *handle = nullptr;
    sqlite3_open_v2(db.c_str(), &handle, SQLITE_OPEN_READONLY, nullptr);
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(handle, sql.c_str(), -1, &stmt, nullptr);
    std::vector<std::byte> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto *bytes = static_cast<const std::byte *>(sqlite3_column_blob(stmt, 0));
        out.assign(bytes, bytes + sqlite3_column_bytes(stmt, 0));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);
    return out;
}

// What one save with the options did: the summary, and the stick before
// and after.
struct AnalysisRun
{
    testing::EngineChangeStick stick;
    QVariantMap summary;
    std::map<std::int64_t, std::vector<std::string>> before;
    std::map<std::int64_t, std::vector<std::string>> after;
    testing::DatabaseDump dumpBefore;
    QStringList pending;
    // Each staged row's text on the page, by the row's key.
    std::map<QString, QString> stagedText;
};

// The staged rows' texts of every run, switches on or off: the same
// proposal staged the same way, so the same text on every row.
std::vector<std::pair<std::string, std::map<QString, QString>>> g_stagedTexts;

int ridersIn(const QStringList &pending);

AnalysisRun runSyncWith(const std::string &name, std::optional<std::pair<bool, bool>> options, bool analyseB01,
                        bool lockB01, RekordboxExportSyncController &controller, bool experimental = true)
{
    AnalysisRun run;
    run.stick = makeStick(name, "m-before-R3.db", "rekordbox-baseline-export40.tsv.gz");
    if (analyseB01) {
        pretendPlayerAnalysedB01(run.stick, lockB01);
    }
    run.before = analysisRows(run.stick.db);
    run.dumpBefore = testing::dumpDatabase(run.stick.db);
    SyncAnalysisPolicy::reset();
    if (options) {
        SyncAnalysisPolicy::set(options->first, options->second);
    }
    // Experimental: the switches count only with the gate open.
    SyncAnalysisPolicy::setExperimentalEnabled(experimental);
    analyze(controller, run.stick);
    assertGoldenProposal(controller);
    controller.stageSelected();
    assert(controller.errorMessage().isEmpty());
    assert(controller.stagedCount() == 24);
    LibraryEditSession *session = sessionOf(run.stick);
    run.pending = session->pendingDescriptions();
    // Every staged row says its own change: one of the save's lines, never
    // an analysis rider's (the riders sit among the changes, before the
    // deletes and removals, and stage no row of their own).
    {
        RekordboxExportSyncListModel *model = controller.rows();
        int staged = 0;
        for (int r = 0; r < model->rowCount(); ++r) {
            const QModelIndex index = model->index(r, 0);
            if (!model->data(index, RekordboxExportSyncListModel::StagedRole).toBool()) {
                continue;
            }
            ++staged;
            const QString key = model->data(index, RekordboxExportSyncListModel::KeyRole).toString();
            const QString text = model->data(index, RekordboxExportSyncListModel::StagedDescriptionRole).toString();
            if (!run.pending.contains(text) || ridersIn({text}) != 0) {
                std::cout << "  staged row " << key.toStdString() << " says \"" << text.toStdString() << "\"\n";
            }
            assert(!text.isEmpty() && run.pending.contains(text) && ridersIn({text}) == 0);
            run.stagedText[key] = text;
        }
        assert(staged == 24);
        g_stagedTexts.emplace_back(name, run.stagedText);
    }
    run.summary = waitForSave(session, controller, false);
    // The sync's own count, whatever the options: the analysis is said
    // beside it, never in it.
    assert(run.summary.value("written").toInt() == 16 && run.summary.value("total").toInt() == 16);
    assert(run.summary.value("unit").toString() == QStringLiteral("changes"));
    assert(controller.proposalEmpty());
    run.after = analysisRows(run.stick.db);
    std::cout << "  notes: \"" << run.summary.value("notes").toString().toStdString() << "\"\n";
    return run;
}

int ridersIn(const QStringList &pending)
{
    return static_cast<int>(std::count_if(pending.begin(), pending.end(), [](const QString &line) {
        return line.startsWith(QStringLiteral("Copy rekordbox's"));
    }));
}

void testAnalysisBoth()
{
    RekordboxExportSyncController controller;
    AnalysisRun run = runSyncWith("t73_analysis_both", std::make_pair(true, true), true, false, controller);
    const auto &stick = run.stick;
    assert(ridersIn(run.pending) == 4 && "N1, N2, N3 and B01, once each");
    const std::int64_t b01 = trackIdOf(stick, "B01");
    const std::vector<std::int64_t> created = {trackIdOf(stick, "N1"), trackIdOf(stick, "N2"), trackIdOf(stick, "N3")};
    for (const std::int64_t id : created) {
        assert(!run.before.count(id) && "made by this save");
    }

    // The created rows: completed, over their files' exact length.
    int completed = 0;
    for (const std::int64_t id : created) {
        const auto &row = run.after.at(id);
        assert(row[IsAnalyzed] == "1" && row[TrackData] != "<null>" && row[Overview] != "<null>"
               && row[BeatData] != "<null>");
        ++completed;
        const auto file = testing::sqlRows(stick.db, "SELECT path FROM Track WHERE id = " + std::to_string(id) + ";");
        const fs::path audio = fs::weakly_canonical(stick.engine / pathFromUtf8(file.at(0).at(0)));
        const auto exact = seabass::infrastructure::audio::readExactSampleCount(pathToUtf8(audio));
        assert(exact && "the head keeps its container's count");
        const auto record = djinterop::engine::v3::track_data_blob::from_blob(
            blobOf(stick.db, "SELECT trackData FROM PerformanceData WHERE trackId = " + std::to_string(id) + ";"));
        std::cout << "  created row " << id << ": " << record.samples << " samples at " << record.sample_rate
                  << " Hz, the file " << exact->samples << " at " << exact->sampleRateHz << "\n";
        assert(record.samples == static_cast<double>(exact->samples));
        assert(record.sample_rate == exact->sampleRateHz);
    }
    // B01, analysed by the player: rekordbox's grid and waveform, the
    // record the player wrote kept.
    {
        const auto &was = run.before.at(b01);
        const auto &now = run.after.at(b01);
        assert(now[BeatData] != was[BeatData] && "rekordbox's grid");
        assert(now[Overview] != was[Overview] && "rekordbox's waveform");
        assert(now[TrackData] == was[TrackData] && now[IsAnalyzed] == "1" && "the player's record stays");
    }
    // Every other row as it was (B05, B06, B09 and B10 the sync removes).
    int removed = 0;
    for (const auto &[id, row] : run.before) {
        if (!run.after.count(id)) {
            ++removed;
        } else if (id != b01) {
            assert(run.after.at(id) == row);
        }
    }
    assert(removed == 4);
    int grids = 0;
    int waveforms = 0;
    for (const auto &[id, row] : run.after) {
        const bool fresh = !run.before.count(id);
        grids += (fresh ? row[BeatData] != "<null>" : row[BeatData] != run.before.at(id)[BeatData]) ? 1 : 0;
        waveforms += (fresh ? row[Overview] != "<null>" : row[Overview] != run.before.at(id)[Overview]) ? 1 : 0;
    }
    // The overview files of exactly those rows, the player's analysis of
    // B01 kept, and the ledger with the four grids.
    const auto overviews = filesUnder(overviewDirOf(stick));
    std::set<std::int64_t> overviewIds;
    for (const auto &[path, bytes] : overviews) {
        overviewIds.insert(std::stoll(pathToUtf8(pathFromUtf8(path).stem())));
    }
    assert((overviewIds == std::set<std::int64_t>{created[0], created[1], created[2], b01}));
    const auto kept = filesUnder(keptDirOf(stick));
    assert(kept.size() == 1 && "B01's own analysis, and only that");
    std::string ledgerError;
    const auto ledger = seabass::infrastructure::local::readBeatGridLedger(stick.root, &ledgerError);
    assert(ledgerError.empty() && ledger.size() == 4);
    // The summary says what landed, counted from the stick.
    std::cout << "  landed: " << grids << " grid(s), " << waveforms << " waveform(s), " << completed
              << " completed, " << kept.size() << " kept\n";
    assert(grids == 4 && waveforms == 4 && completed == 3);
    assert(run.summary.value("notes").toString()
           == QStringLiteral("From rekordbox, 4 beat grids and 4 waveforms copied, 3 tracks now load without "
                             "analysis. Engine's own analysis of one track was kept on the stick first."));
    std::cout << "analysis both: 4 grids, 4 waveforms, 3 rows completed, B01's analysis kept, the rest as it was OK\n";

    // Undo: m.db as before the save (but the import counter, as the "save"
    // case has it), no overview file, nothing kept, no ledger.
    LibraryEditSession *session = sessionOf(stick);
    waitForSave(session, controller, true);
    {
        testing::DatabaseDump after = testing::dumpDatabase(stick.db);
        testing::DatabaseDump before = run.dumpBefore;
        before.erase("Information");
        after.erase("Information");
        const auto diffs = testing::differences(before, after);
        testing::printDifferences(diffs);
        assert(diffs.empty());
    }
    assert(filesUnder(overviewDirOf(stick)).empty());
    assert(filesUnder(keptDirOf(stick)).empty());
    assert(!fs::exists(seabass::infrastructure::paths::stickBeatGridLedger(stick.root)));
    std::cout << "analysis undo: m.db, overview files, kept analysis and ledger as before OK\n";
}

void testAnalysisLocked()
{
    RekordboxExportSyncController controller;
    AnalysisRun run = runSyncWith("t73_analysis_locked", std::make_pair(true, true), true, true, controller);
    const std::int64_t b01 = trackIdOf(run.stick, "B01");
    const auto &was = run.before.at(b01);
    const auto &now = run.after.at(b01);
    assert(now[BeatData] == was[BeatData] && now[Locked] == "1" && "a locked grid is never replaced by a sync");
    assert(now[Overview] != was[Overview] && "the waveform has no lock");
    assert(run.summary.value("notes").toString()
           == QStringLiteral("From rekordbox, 3 beat grids and 4 waveforms copied, 3 tracks now load without "
                             "analysis. Engine's own analysis of one track was kept on the stick first. Analysis "
                             "left alone: one beat grid locked or moved on the player."));
    std::cout << "analysis locked: B01's grid left alone and counted, its waveform copied OK\n";
}

void testAnalysisGridsOnly()
{
    RekordboxExportSyncController controller;
    AnalysisRun run = runSyncWith("t73_analysis_grids", std::make_pair(true, false), true, false, controller);
    const auto &stick = run.stick;
    assert(ridersIn(run.pending) == 4);
    const std::int64_t b01 = trackIdOf(stick, "B01");
    for (const char *name : {"N1", "N2", "N3"}) {
        const auto &row = run.after.at(trackIdOf(stick, name));
        assert(row[IsAnalyzed] == "0" && row[TrackData] == "<null>" && row[BeatData] == "<null>"
               && "left for the player: a row is completed with both or not at all");
    }
    assert(run.after.at(b01)[BeatData] != run.before.at(b01)[BeatData]);
    assert(run.after.at(b01)[Overview] == run.before.at(b01)[Overview] && "no waveform asked for");
    assert(filesUnder(overviewDirOf(stick)).empty());
    assert(filesUnder(keptDirOf(stick)).size() == 1);
    assert(run.summary.value("notes").toString()
           == QStringLiteral("From rekordbox, one beat grid copied. Engine's own analysis of one track was kept on "
                             "the stick first. Analysis left alone: 3 tracks left for the player to analyse."));
    std::cout << "analysis grids only: B01's grid, the 3 new rows left for the player OK\n";
}

// The stick after a save, but what differs between two runs of the same
// save whatever they do: the clock in Playlist and Track, the backups'
// own ids and dates, and the log's times.
struct StickState
{
    testing::DatabaseDump dump;
    std::vector<std::vector<std::string>> playlists;
    std::vector<std::vector<std::string>> tracks;
    std::map<std::string, std::string> engineFiles;
    std::map<std::string, std::string> pioneerFiles;
    std::map<std::string, std::string> contents;
    std::set<std::string> seabassFiles;
    std::string baseline;
};

StickState stateOf(const testing::EngineChangeStick &stick)
{
    StickState state;
    state.dump = testing::dumpDatabase(stick.db);
    state.dump.erase("Playlist");
    state.dump.erase("Track");
    state.playlists = rowsButClock(stick.db, "Playlist");
    state.tracks = rowsButClock(stick.db, "Track");
    for (auto [path, bytes] : filesUnder(stick.engine)) {
        if (path.rfind("Database2/m.db", 0) != 0) {
            state.engineFiles[path] = std::move(bytes);
        }
    }
    state.pioneerFiles = filesUnder(stick.pioneer);
    state.contents = filesUnder(stick.root / "Contents");
    for (const auto &[path, bytes] : filesUnder(stick.root / "Seabass")) {
        state.seabassFiles.insert(path);
    }
    // The record as written, but the second it was written in: read, the
    // time cleared, written again elsewhere. Two runs a second apart
    // otherwise differ there and nowhere else.
    {
        std::string error;
        auto record = seabass::infrastructure::local::readRekordboxBaseline(stick.root, &error);
        assert(record && error.empty());
        record->recordedAtUnix = 0;
        const fs::path elsewhere = stick.root.parent_path() / (pathToUtf8(stick.root.filename()) + "-record");
        fs::remove_all(elsewhere);
        fs::create_directories(elsewhere);
        std::string writeError;
        const bool written = seabass::infrastructure::local::writeRekordboxBaseline(elsewhere, *record, [](const std::string &) {}, &writeError);
        assert(written);
        (void)written;
        state.baseline = fileBytes(seabass::infrastructure::paths::stickRekordboxBaseline(elsewhere));
    }
    return state;
}

void testAnalysisOff()
{
    StickState never;
    StickState off;
    {
        RekordboxExportSyncController controller;
        AnalysisRun run = runSyncWith("t73_analysis_never", std::nullopt, false, false, controller);
        assert(ridersIn(run.pending) == 0);
        assert(run.summary.value("notes").toString().isEmpty());
        never = stateOf(run.stick);
    }
    {
        RekordboxExportSyncController controller;
        AnalysisRun run = runSyncWith("t73_analysis_off", std::make_pair(false, false), false, false, controller);
        assert(ridersIn(run.pending) == 0);
        assert(run.summary.value("notes").toString().isEmpty());
        off = stateOf(run.stick);
    }
    // Both switches stored on, Experimental off: the switches have no
    // effect (SyncAnalysisPolicy reads both off), the same save again.
    StickState gated;
    {
        RekordboxExportSyncController controller;
        AnalysisRun run =
            runSyncWith("t73_analysis_gated", std::make_pair(true, true), false, false, controller, false);
        assert(ridersIn(run.pending) == 0 && "no rider while Experimental is off");
        assert(run.summary.value("notes").toString().isEmpty());
        gated = stateOf(run.stick);
    }
    for (const StickState *other : {&off, &gated}) {
        const auto diffs = testing::differences(never.dump, other->dump);
        testing::printDifferences(diffs);
        assert(diffs.empty());
        assert(never.playlists == other->playlists && never.tracks == other->tracks);
        assert(never.engineFiles == other->engineFiles && never.pioneerFiles == other->pioneerFiles);
        for (const auto &[path, bytes] : never.contents) {
            if (!other->contents.count(path) || other->contents.at(path) != bytes) {
                std::cout << "  differs: Contents/" << path << "\n";
            }
        }
        if (never.baseline != other->baseline) {
            std::cout << "  differs: the record\n";
        }
        assert(never.contents == other->contents && never.baseline == other->baseline);
    }
    // The same files under Seabass/, but the backups' own names.
    const auto named = [](const std::set<std::string> &files) {
        std::multiset<std::string> out;
        for (const auto &f : files) {
            out.insert(f.rfind("backups/", 0) == 0 ? std::string("backups/<record>") : f);
        }
        return out;
    };
    assert(named(never.seabassFiles) == named(off.seabassFiles));
    assert(named(never.seabassFiles) == named(gated.seabassFiles));
    for (const auto &f : off.seabassFiles) {
        assert(f.find("engine-analysis") == std::string::npos && f.find("beat-grids") == std::string::npos);
    }
    for (const auto &[path, bytes] : off.engineFiles) {
        assert(path.rfind("Database2/OverviewData", 0) != 0 && "no overview file written");
    }
    std::cout << "analysis off: never set, both off, and both on with Experimental off write the same m.db and the same stick OK\n";
}

void testAnalysis()
{
#ifdef SEABASS_HAVE_TAGLIB
    testAnalysisBoth();
    testAnalysisLocked();
    testAnalysisGridsOnly();
    testAnalysisOff();
    SyncAnalysisPolicy::reset();
    // Riders or none, every run put the same text on the same rows.
    assert(g_stagedTexts.size() >= 2);
    for (const auto &[name, texts] : g_stagedTexts) {
        for (const auto &[key, text] : g_stagedTexts.front().second) {
            if (!texts.count(key) || texts.at(key) != text) {
                std::cout << "  " << name << ": row " << key.toStdString() << " says \""
                          << (texts.count(key) ? texts.at(key).toStdString() : std::string("<not staged>"))
                          << "\", " << g_stagedTexts.front().first << " \"" << text.toStdString() << "\"\n";
            }
        }
        assert(texts == g_stagedTexts.front().second);
    }
    std::cout << "analysis staged rows: every row its own change's text, riders or none, in "
              << g_stagedTexts.size() << " runs OK\n";
#else
    std::cerr << "analysis: needs TagLib (a row is completed from its file's own sample count)\n";
    std::exit(1);
#endif
}


// --- Sync Cue Points with rekordbox's analysis ------------------------------

using seabass::gui::SyncController;

std::int64_t trackIdOfDb(const std::string &db, const std::string &name)
{
    return testing::sqlScalar(db, "SELECT id FROM Track WHERE filename LIKE 'T73-" + name + "-%';");
}

void waitUntilIdle(SyncController &controller)
{
    QSignalSpy busy(&controller, &SyncController::busyChanged);
    for (int i = 0; i < 3000 && controller.busy(); ++i) {
        busy.wait(100);
    }
    assert(!controller.busy());
}

void analyze(SyncController &controller, const testing::EngineChangeStick &stick)
{
    controller.analyze(stick.pioneerPath(), stick.enginePath());
    waitUntilIdle(controller);
    if (!controller.errorMessage().isEmpty()) {
        std::cerr << "sync analysis: " << controller.errorMessage().toStdString() << "\n";
    }
    assert(controller.errorMessage().isEmpty());
}

// A plan's row as the page reads it: `role` of the row whose planIndex is
// `planIndex`, invalid when the search hides it.
QVariant rowRole(SyncController &controller, int planIndex, int role)
{
    auto *model = controller.plansModel();
    for (int r = 0; r < model->rowCount(); ++r) {
        const QModelIndex index = model->index(r);
        if (model->data(index, seabass::gui::SyncPlanListModel::PlanIndexRole).toInt() == planIndex) {
            return model->data(index, role);
        }
    }
    return {};
}

QString analysisTextOf(SyncController &controller, int planIndex)
{
    return rowRole(controller, planIndex, seabass::gui::SyncPlanListModel::AnalysisRole).toString();
}

void printPlans(SyncController &controller)
{
    const auto &plans = controller.plansModel()->plans();
    for (std::size_t i = 0; i < plans.size(); ++i) {
        const auto &plan = plans[i];
        const bool toB = plan.direction == d::SyncPlan::Direction::ToB;
        const auto &src = toB ? plan.match.trackA : plan.match.trackB;
        const auto &tgt = toB ? plan.match.trackB : plan.match.trackA;
        const int index = static_cast<int>(i);
        std::cout << "  " << (controller.plansModel()->isAnalysisOnly(index) ? "analysis only" : "plan") << ": "
                  << src.format << " " << src.filename << " -> " << tgt.format << " id=" << tgt.sourceId << " ("
                  << plan.cuesToApply.size() << " cues) \""
                  << analysisTextOf(controller, index).toStdString() << "\"\n";
    }
}

int cueRowCount(SyncController &controller)
{
    return controller.planCount() - controller.analysisOnlyCount();
}

// Engine's cues of `names` taken away (N3's cue list, which holds none of
// theirs), so Sync Cue Points proposes rekordbox's onto Engine; and
// `toRekordbox` given B02's on Engine, so that row goes the other way.
void makeCueRows(const testing::EngineChangeStick &stick, const std::vector<std::string> &names,
                 const std::string &toRekordbox)
{
    const std::string n3 = std::to_string(trackIdOf(stick, "N3"));
    for (const auto &name : names) {
        const std::string id = std::to_string(trackIdOf(stick, name));
        const bool done = testing::sqlExec(
            stick.db, "UPDATE PerformanceData SET quickCues = (SELECT quickCues FROM PerformanceData WHERE trackId = "
                          + n3 + "), loops = (SELECT loops FROM PerformanceData WHERE trackId = " + n3
                          + ") WHERE trackId = " + id + ";");
        assert(done);
        (void)done;
    }
    if (!toRekordbox.empty()) {
        const std::string b02 = std::to_string(trackIdOf(stick, "B02"));
        const std::string id = std::to_string(trackIdOf(stick, toRekordbox));
        const bool done = testing::sqlExec(
            stick.db, "UPDATE PerformanceData SET quickCues = (SELECT quickCues FROM PerformanceData WHERE trackId = "
                          + b02 + ") WHERE trackId = " + id + ";");
        assert(done);
        (void)done;
    }
}

QVariantMap waitForSyncSave(LibraryEditSession *session, SyncController &controller, bool undo)
{
    QSignalSpy finished(session, &LibraryEditSession::saveFinished);
    if (undo) {
        controller.undoLastOperation();
    } else {
        session->save();
    }
    for (int i = 0; i < 1200 && finished.isEmpty(); ++i) {
        finished.wait(100);
    }
    assert(!finished.isEmpty() && "the save finished");
    waitUntilIdle(controller);
    const QVariantMap summary = finished.takeFirst().at(0).toMap();
    std::cout << "  " << (undo ? "undo" : "save") << ": " << summary.value("written").toInt() << " of "
              << summary.value("total").toInt() << " " << summary.value("unit").toString().toStdString()
              << "; error \"" << summary.value("error").toString().toStdString() << "\", notes \""
              << summary.value("notes").toString().toStdString() << "\"\n";
    assert(summary.value("error").toString().isEmpty());
    assert(summary.value("warning").toString().isEmpty());
    return summary;
}

int planIndexOf(SyncController &controller, const std::string &name)
{
    const auto &plans = controller.plansModel()->plans();
    for (std::size_t i = 0; i < plans.size(); ++i) {
        const auto &m = plans[i].match;
        if (m.trackA.filename.rfind("T73-" + name + "-", 0) == 0 || m.trackB.filename.rfind("T73-" + name + "-", 0) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string quickCuesOf(const testing::EngineChangeStick &stick, const std::string &name)
{
    return testing::sqlRows(stick.db, "SELECT hex(quickCues) || '/' || hex(loops) FROM PerformanceData WHERE trackId = "
                                          + std::to_string(trackIdOf(stick, name)) + ";")
        .at(0)
        .at(0);
}

// One Sync Cue Points run: B04 and B11 (analysed by the player) and B03
// (not) lose their cues on Engine, so their rekordbox cues go to Engine;
// N3 gains cues on Engine, so its row goes to rekordbox. B11's grid is
// locked on the player. `experimental`: the gate, the switches as
// `options` (none: never set). The rows of tracks that need only
// rekordbox's analysis are unticked (the rider cases here are about the
// cue rows; testCueSyncAnalysisOnly is about those), and so are the cue
// rows named in `untick`; those in `unstage` are unstaged after staging.
struct CueSyncRun
{
    testing::EngineChangeStick stick;
    std::map<std::int64_t, std::vector<std::string>> before;
    testing::DatabaseDump dumpBefore;
    std::map<std::string, std::string> cuesBefore;
    QStringList pending;
    QString staged;
    QVariantMap summary;
    std::map<std::int64_t, std::vector<std::string>> after;
    // What each cue row said of rekordbox's analysis (AnalysisRole), read
    // before the save takes the rows away.
    std::map<std::string, QString> rowAnalysis;
};

const std::vector<std::string> CueRowsToEngine = {"B03", "B04", "B11"};

// `prepare` runs on the stick before anything is read, `beforeSave` between
// staging and the save.
CueSyncRun runCueSync(const std::string &name, std::optional<std::pair<bool, bool>> options, bool experimental,
                      SyncController &controller, const std::vector<std::string> &unstage = {},
                      const std::vector<std::string> &untick = {},
                      const std::function<void(const testing::EngineChangeStick &)> &prepare = {},
                      const std::function<void()> &beforeSave = {})
{
    CueSyncRun run;
    run.stick = makeStick(name, "m-after-R3c-save1.db", "rekordbox-baseline-export128.tsv.gz");
    makeCueRows(run.stick, CueRowsToEngine, "N3");
    if (prepare) {
        prepare(run.stick);
    }
    const bool locked = testing::sqlExec(run.stick.db, "UPDATE Track SET isBeatGridLocked = 1 WHERE id = "
                                                           + std::to_string(trackIdOf(run.stick, "B11")) + ";");
    assert(locked);
    (void)locked;
    run.before = analysisRows(run.stick.db);
    run.dumpBefore = testing::dumpDatabase(run.stick.db);
    for (const char *track : {"B03", "B04", "B11", "N3", "B01"}) {
        run.cuesBefore[track] = quickCuesOf(run.stick, track);
    }
    SyncAnalysisPolicy::reset();
    if (options) {
        SyncAnalysisPolicy::set(options->first, options->second);
    }
    SyncAnalysisPolicy::setExperimentalEnabled(experimental);
    analyze(controller, run.stick);
    printPlans(controller);
    // B03, B04 and B11 to Engine, N3 to rekordbox, no other cue row.
    assert(cueRowCount(controller) == 4 && controller.conflictCount() == 0);
    for (const auto &track : CueRowsToEngine) {
        const int i = planIndexOf(controller, track);
        assert(i >= 0);
        const auto &plan = controller.plansModel()->plans()[static_cast<std::size_t>(i)];
        assert(plan.direction == d::SyncPlan::Direction::ToB && plan.match.trackB.format == "engine");
    }
    {
        const auto &plan = controller.plansModel()->plans()[static_cast<std::size_t>(planIndexOf(controller, "N3"))];
        const auto &target = plan.direction == d::SyncPlan::Direction::ToB ? plan.match.trackB : plan.match.trackA;
        assert(target.format == "rekordbox" && "N3 goes the other way");
    }
    // One row per track: a row whose cues go to Engine carries its
    // analysis as a rider and is never offered for its analysis alone as
    // well, and N3, whose cues go the other way, is its cue row only.
    for (const auto &track : {"B03", "B04", "B11", "N3"}) {
        const std::string id = std::to_string(trackIdOf(run.stick, track));
        int rows = 0;
        for (const auto &plan : controller.plansModel()->plans()) {
            // trackIdOf is Engine's id: compared with Engine copies only.
            for (const auto *track : {&plan.match.trackA, &plan.match.trackB}) {
                rows += track->format == "engine" && track->sourceId == id ? 1 : 0;
            }
        }
        assert(rows == 1 && "never both a rider and a change of its own");
    }
    for (const char *track : {"B03", "B04", "B11", "N3"}) {
        run.rowAnalysis[track] = analysisTextOf(controller, planIndexOf(controller, track));
    }
    assert(run.rowAnalysis.at("N3").isEmpty() && "N3's cues go to rekordbox");
    for (int i = 0; i < controller.planCount(); ++i) {
        if (controller.plansModel()->isAnalysisOnly(i)) {
            controller.setIncluded(i, false);
        }
    }
    for (const auto &track : untick) {
        controller.setIncluded(planIndexOf(controller, track), false);
    }
    assert(controller.selectedCount() == static_cast<int>(4 - untick.size()));
    controller.stageSelected(false);
    assert(controller.errorMessage().isEmpty());
    run.staged = controller.statusMessage();
    std::cout << "  staged: \"" << run.staged.toStdString() << "\"\n";
    assert(controller.stagedCount() == static_cast<int>(4 - untick.size()) && "an unticked row is not staged");
    for (const auto &track : unstage) {
        controller.unstage(planIndexOf(controller, track));
    }
    LibraryEditSession *session = sessionOf(run.stick);
    run.pending = session->pendingDescriptions();
    // One staged change per row, rider or not.
    const int rows = static_cast<int>(4 - unstage.size() - untick.size());
    assert(run.pending.size() == rows);
    assert(ridersIn(run.pending) == 0 && "the analysis rides inside the row's own change");
    if (beforeSave) {
        beforeSave();
    }
    run.summary = waitForSyncSave(session, controller, false);
    // The rows' own count, whatever the switches.
    assert(run.summary.value("written").toInt() == rows);
    assert(run.summary.value("verb").toString() == QStringLiteral("synchronised"));
    assert(run.summary.value("unit").toString() == QStringLiteral("tracks"));
    run.after = analysisRows(run.stick.db);
    return run;
}

void assertCuesLanded(const CueSyncRun &run, const std::vector<std::string> &tracks)
{
    for (const auto &track : tracks) {
        assert(quickCuesOf(run.stick, track) != run.cuesBefore.at(track) && "rekordbox's cues are on Engine");
    }
}

void testCueSyncBoth()
{
    SyncController controller;
    CueSyncRun run = runCueSync("t73_cuesync_both", std::make_pair(true, true), true, controller);
    const auto &stick = run.stick;
    assert(run.staged == QStringLiteral("Staged 4 track(s). Press Save to write the cues to the stick. The tracks whose "
                                        "cues go to Engine also get rekordbox's beat grids and waveforms, as set in "
                                        "Preferences."));
    // Each cue row says what its Engine copy gets, as the save then writes
    // it: B11's locked grid is not among it.
    assert(run.rowAnalysis.at("B04") == QStringLiteral("Beat grid and waveform from rekordbox"));
    assert(run.rowAnalysis.at("B11") == QStringLiteral("Waveform from rekordbox"));
    assert(run.rowAnalysis.at("B03")
           == QStringLiteral("Beat grid and waveform from rekordbox, then loads without analysis on the player"));
    assert(run.staged.contains(QStringLiteral("The tracks whose cues go to Engine also get rekordbox's beat grids and "
                                              "waveforms, as set in Preferences.")));
    assertCuesLanded(run, CueRowsToEngine);
    const std::int64_t b03 = trackIdOf(stick, "B03");
    const std::int64_t b04 = trackIdOf(stick, "B04");
    const std::int64_t b11 = trackIdOf(stick, "B11");
    const std::int64_t n3 = trackIdOf(stick, "N3");
    // B04, analysed by the player: rekordbox's grid and waveform, its own
    // record stays.
    assert(run.after.at(b04)[BeatData] != run.before.at(b04)[BeatData]);
    assert(run.after.at(b04)[Overview] != run.before.at(b04)[Overview]);
    assert(run.after.at(b04)[TrackData] == run.before.at(b04)[TrackData]);
    // B11, its grid locked: the grid left alone, the waveform copied.
    assert(run.after.at(b11)[BeatData] == run.before.at(b11)[BeatData] && "a locked grid is never replaced");
    assert(run.after.at(b11)[Overview] != run.before.at(b11)[Overview]);
    // B03, not analysed: completed over its file's own length.
    {
        const auto &row = run.after.at(b03);
        assert(row[IsAnalyzed] == "1" && row[TrackData] != "<null>" && row[BeatData] != "<null>");
        const auto file = testing::sqlRows(stick.db, "SELECT path FROM Track WHERE id = " + std::to_string(b03) + ";");
        const fs::path audio = fs::weakly_canonical(stick.engine / pathFromUtf8(file.at(0).at(0)));
        const auto exact = seabass::infrastructure::audio::readExactSampleCount(pathToUtf8(audio));
        const auto record = djinterop::engine::v3::track_data_blob::from_blob(
            blobOf(stick.db, "SELECT trackData FROM PerformanceData WHERE trackId = " + std::to_string(b03) + ";"));
        assert(exact && record.samples == static_cast<std::int64_t>(exact->samples));
    }
    // Every other row as it was, N3 (its cues went to rekordbox) included.
    for (const auto &[id, row] : run.before) {
        if (id != b03 && id != b04 && id != b11) {
            assert(run.after.at(id) == row);
        }
    }
    assert(quickCuesOf(stick, "N3") == run.cuesBefore.at("N3") && "N3's Engine row is not written");
    (void)n3;
    std::set<std::int64_t> overviewIds;
    for (const auto &[path, bytes] : filesUnder(overviewDirOf(stick))) {
        overviewIds.insert(std::stoll(pathToUtf8(pathFromUtf8(path).stem())));
    }
    assert((overviewIds == std::set<std::int64_t>{b03, b04, b11}));
    assert(filesUnder(keptDirOf(stick)).size() == 2 && "B04's and B11's own analysis kept");
    std::string ledgerError;
    const auto ledger = seabass::infrastructure::local::readBeatGridLedger(stick.root, &ledgerError);
    assert(ledgerError.empty() && ledger.size() == 2 && "the grids written: B03's and B04's");
    // B11's rider carries what its row said, the waveform: its locked
    // grid is not asked for, so not "left alone" in the note either.
    assert(run.summary.value("notes").toString()
           == QStringLiteral("From rekordbox, 2 beat grids and 3 waveforms copied, one track now loads without "
                             "analysis. Engine's own analysis of 2 tracks was kept on the stick first."));
    std::cout << "cue sync both: B03 completed, B04 replaced and kept, B11's locked grid left alone, N3 untouched OK\n";

    // Undo: m.db, overview files, kept analyses and ledger as before.
    waitForSyncSave(sessionOf(stick), controller, true);
    {
        testing::DatabaseDump after = testing::dumpDatabase(stick.db);
        testing::DatabaseDump before = run.dumpBefore;
        before.erase("Information");
        after.erase("Information");
        const auto diffs = testing::differences(before, after);
        testing::printDifferences(diffs);
        assert(diffs.empty());
    }
    assert(filesUnder(overviewDirOf(stick)).empty());
    assert(filesUnder(keptDirOf(stick)).empty());
    assert(!fs::exists(seabass::infrastructure::paths::stickBeatGridLedger(stick.root)));
    std::cout << "cue sync undo: m.db, overview files, kept analyses and ledger as before OK\n";
}

// A row unstaged takes its analysis with it: B04 as it was, cues and all.
void testCueSyncUnstage()
{
    SyncController controller;
    CueSyncRun run = runCueSync("t73_cuesync_unstage", std::make_pair(true, true), true, controller, {"B04"});
    const std::int64_t b04 = trackIdOf(run.stick, "B04");
    assert(run.after.at(b04) == run.before.at(b04) && "no analysis without its row");
    assert(quickCuesOf(run.stick, "B04") == run.cuesBefore.at("B04"));
    assertCuesLanded(run, {"B03", "B11"});
    std::set<std::int64_t> overviewIds;
    for (const auto &[path, bytes] : filesUnder(overviewDirOf(run.stick))) {
        overviewIds.insert(std::stoll(pathToUtf8(pathFromUtf8(path).stem())));
    }
    assert(!overviewIds.count(b04) && overviewIds.size() == 2);
    assert(filesUnder(keptDirOf(run.stick)).size() == 1 && "B11's only");
    std::cout << "cue sync unstage: B04's analysis left with its row OK\n";
}

// A row unticked means "do not touch this track": B04 gets neither its
// cues nor rekordbox's analysis, and is not offered for its analysis
// alone either (one row per track). Its row is still listed afterwards.
void testCueSyncUntick()
{
    SyncController controller;
    CueSyncRun run = runCueSync("t73_cuesync_untick", std::make_pair(true, true), true, controller, {}, {"B04"});
    const std::int64_t b04 = trackIdOf(run.stick, "B04");
    assert(run.rowAnalysis.at("B04") == QStringLiteral("Beat grid and waveform from rekordbox"));
    assert(run.after.at(b04) == run.before.at(b04) && "neither cues nor analysis for an unticked row");
    assert(quickCuesOf(run.stick, "B04") == run.cuesBefore.at("B04"));
    assertCuesLanded(run, {"B03", "B11"});
    for (const auto &[path, bytes] : filesUnder(overviewDirOf(run.stick))) {
        assert(std::stoll(pathToUtf8(pathFromUtf8(path).stem())) != b04);
    }
    const int row = planIndexOf(controller, "B04");
    assert(row >= 0 && !controller.plansModel()->isAnalysisOnly(row) && !controller.plansModel()->isStaged(row));
    std::cout << "cue sync untick: B04 left whole, still listed OK\n";
}

// Grids only: B04's grid, B03 left for the player.
void testCueSyncGridsOnly()
{
    SyncController controller;
    CueSyncRun run = runCueSync("t73_cuesync_grids", std::make_pair(true, false), true, controller);
    assert(run.staged.contains(QStringLiteral("also get rekordbox's beat grids, as set in Preferences.")));
    assert(run.rowAnalysis.at("B04") == QStringLiteral("Beat grid from rekordbox"));
    assert(run.rowAnalysis.at("B11").isEmpty() && "a locked grid: nothing to say");
    assert(run.rowAnalysis.at("B03").isEmpty() && "a row is completed with both or not at all");
    const std::int64_t b03 = trackIdOf(run.stick, "B03");
    assert(run.after.at(b03) == run.before.at(b03) && "a row is completed with both or not at all");
    assert(run.summary.value("notes").toString()
           == QStringLiteral("From rekordbox, one beat grid copied. Engine's own analysis of one track was kept on the "
                             "stick first."));
    // B11 and B03, whose rows said no analysis, carried none: nothing of
    // theirs was asked for, so nothing is said "left alone".
    std::cout << "cue sync grids only: B04's grid, B03 left for the player, B11 locked OK\n";
}

// Off, never set, and both on with Experimental off: the same save.
void testCueSyncOff()
{
    std::vector<StickState> states;
    for (const auto &[name, options, experimental] :
         {std::tuple{"t73_cuesync_never", std::optional<std::pair<bool, bool>>{}, true},
          std::tuple{"t73_cuesync_off", std::optional<std::pair<bool, bool>>{std::make_pair(false, false)}, true},
          std::tuple{"t73_cuesync_gated", std::optional<std::pair<bool, bool>>{std::make_pair(true, true)}, false}}) {
        SyncController controller;
        CueSyncRun run = runCueSync(name, options, experimental, controller);
        assert(controller.analysisOnlyCount() == 0 && "no track offered for its analysis alone");
        for (const auto &[track, text] : run.rowAnalysis) {
            assert(text.isEmpty() && "no row speaks of rekordbox's analysis");
        }
        assert(!run.staged.contains(QStringLiteral("rekordbox's")) && "the staged line as it always was");
        assert(run.summary.value("notes").toString().isEmpty());
        assertCuesLanded(run, CueRowsToEngine);
        assert(run.after == run.before && "no row's analysis changes");
        assert(filesUnder(overviewDirOf(run.stick)).empty());
        assert(filesUnder(keptDirOf(run.stick)).empty());
        assert(!fs::exists(seabass::infrastructure::paths::stickBeatGridLedger(run.stick.root)));
        states.push_back(stateOf(run.stick));
    }
    for (std::size_t i = 1; i < states.size(); ++i) {
        const auto diffs = testing::differences(states[0].dump, states[i].dump);
        testing::printDifferences(diffs);
        assert(diffs.empty());
        assert(states[0].tracks == states[i].tracks && states[0].engineFiles == states[i].engineFiles);
        // exportLibrary.db is SQLCipher: N3's cues written into it make
        // different bytes on every write, whatever the switches (its cue
        // write is not this change's); every other file the same.
        auto pioneerA = states[0].pioneerFiles;
        auto pioneerB = states[i].pioneerFiles;
        pioneerA.erase("rekordbox/exportLibrary.db");
        pioneerB.erase("rekordbox/exportLibrary.db");
        assert(pioneerA == pioneerB && states[0].contents == states[i].contents);
    }
    std::cout << "cue sync off: never set, both off and Experimental off write the same stick OK\n";
}

fs::path anlzDatOf(const fs::path &root, const std::string &name);

// The fixture's stick after the R3c save, every track's cues in step, B11's
// grid locked on the player and `unanalysed` made a row the player has not
// analysed (its analysis taken away, as a new row has none).
struct InStepStick
{
    testing::EngineChangeStick stick;
    std::map<std::int64_t, std::vector<std::string>> before;
    testing::DatabaseDump dumpBefore;
};

InStepStick makeInStepStick(const std::string &name, const std::string &unanalysed)
{
    InStepStick out;
    out.stick = makeStick(name, "m-after-R3c-save1.db", "rekordbox-baseline-export128.tsv.gz");
    const std::string b11 = std::to_string(trackIdOf(out.stick, "B11"));
    const std::string row = std::to_string(trackIdOf(out.stick, unanalysed));
    const bool done = testing::sqlExec(
        out.stick.db, "UPDATE Track SET isBeatGridLocked = 1 WHERE id = " + b11
                          + "; UPDATE Track SET isAnalyzed = 0, bpmAnalyzed = NULL WHERE id = " + row
                          + "; UPDATE PerformanceData SET trackData = NULL, overviewWaveFormData = NULL, beatData = "
                            "NULL WHERE trackId = "
                          + row + ";");
    assert(done);
    (void)done;
    out.before = analysisRows(out.stick.db);
    out.dumpBefore = testing::dumpDatabase(out.stick.db);
    return out;
}

// The analysis-only row of `name`'s Engine copy: its plan index, -1 for
// none.
int analysisOnlyOf(SyncController &controller, const testing::EngineChangeStick &stick, const std::string &name)
{
    const std::string id = std::to_string(trackIdOf(stick, name));
    const auto &plans = controller.plansModel()->plans();
    for (std::size_t i = 0; i < plans.size(); ++i) {
        if (controller.plansModel()->isAnalysisOnly(static_cast<int>(i)) && plans[i].match.trackB.sourceId == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// The owner's report: every track's cues in step, both switches on, an
// Engine row the player has not analysed that rekordbox can complete. Sync
// Cue Points lists it (and every other track whose analysis would change)
// as a row of its own, ticked, rekordbox to Engine, saying what it gets;
// the search narrows those rows as it narrows cue rows; an unticked one is
// not written; Save counts them as synced tracks; Undo puts the stick
// back. B11's locked grid is never replaced: it is offered for its
// waveform only.
void testCueSyncAnalysisOnly()
{
    using Model = seabass::gui::SyncPlanListModel;
    InStepStick in = makeInStepStick("t73_cuesync_analysis_only", "B03");
    const auto &stick = in.stick;
    SyncAnalysisPolicy::reset();
    SyncAnalysisPolicy::set(true, true);
    SyncAnalysisPolicy::setExperimentalEnabled(true);
    SyncController controller;
    analyze(controller, stick);
    printPlans(controller);
    assert(cueRowCount(controller) == 0 && controller.conflictCount() == 0 && "every track's cues in step");
    const int offered = controller.analysisOnlyCount();
    // Every Engine row of the fixture but B04 and B11 is one the player has
    // not analysed; B11 offers its waveform only.
    assert(offered == 11);
    // Rows like the others: listed, ticked, counted, rekordbox to Engine.
    assert(controller.planCount() == offered && controller.visiblePlanCount() == offered);
    assert(controller.selectedCount() == offered && controller.plansModel()->rowCount() == offered);
    const int b03 = analysisOnlyOf(controller, stick, "B03");
    const int b04 = analysisOnlyOf(controller, stick, "B04");
    const int b11 = analysisOnlyOf(controller, stick, "B11");
    assert(b03 >= 0 && b04 >= 0 && b11 >= 0);
    for (int i = 0; i < offered; ++i) {
        assert(rowRole(controller, i, Model::AnalysisOnlyRole).toBool());
        assert(rowRole(controller, i, Model::IncludedRole).toBool() && "ticked, as a cue row is");
        assert(rowRole(controller, i, Model::SectionRole).toString() == QStringLiteral("ready"));
        assert(rowRole(controller, i, Model::SourceFormatRole).toString() == QStringLiteral("rekordbox"));
        assert(rowRole(controller, i, Model::TargetFormatRole).toString() == QStringLiteral("engine"));
        assert(rowRole(controller, i, Model::CueSummaryRole).toString() == QStringLiteral("cues in step"));
        const QVariantMap change = rowRole(controller, i, Model::CueChangeRole).toMap();
        for (auto it = change.begin(); it != change.end(); ++it) {
            assert(it.value().toInt() == 0 && "no cue counts invented");
        }
    }
    assert(analysisTextOf(controller, b03)
           == QStringLiteral("Beat grid and waveform from rekordbox, then loads without analysis on the player"));
    assert(analysisTextOf(controller, b04) == QStringLiteral("Beat grid and waveform from rekordbox"));
    assert(analysisTextOf(controller, b11) == QStringLiteral("Waveform from rekordbox") && "a locked grid: never");

    // The search narrows them, and staging with it stages what it shows.
    const auto &b03Track = controller.plansModel()->plans()[static_cast<std::size_t>(b03)].match.trackA;
    controller.search(QString::fromStdString(b03Track.title));
    const int shown = controller.visiblePlanCount();
    std::cout << "  search \"" << b03Track.title << "\": " << shown << " row(s)\n";
    assert(shown >= 1 && shown < offered && controller.selectedVisibleCount() == shown);
    controller.stageSelected(true);
    assert(controller.errorMessage().isEmpty());
    assert(controller.stagedCount() == shown && controller.plansModel()->isStaged(b03));
    assert(controller.plansModel()->isStaged(b03) && !controller.plansModel()->isStaged(b04));
    for (int i = 0; i < offered; ++i) {
        if (controller.plansModel()->isStaged(i)) {
            controller.unstage(i);
        }
    }
    assert(controller.stagedCount() == 0);
    controller.search(QString());
    assert(controller.visiblePlanCount() == offered);

    // B04 unticked: everything else is staged, B04 is not.
    controller.setIncluded(b04, false);
    assert(controller.selectedCount() == offered - 1);
    controller.stageSelected();
    assert(controller.errorMessage().isEmpty());
    std::cout << "  staged: \"" << controller.statusMessage().toStdString() << "\"\n";
    assert(controller.statusMessage()
           == QStringLiteral("Staged rekordbox's beat grids and waveforms for %1 track(s) whose cues are already in "
                             "step. Press Save to write them to the stick.")
                  .arg(offered - 1));
    assert(controller.stagedCount() == offered - 1 && !controller.plansModel()->isStaged(b04));
    assert(controller.selectedCount() == 0);
    // Staging again stages nothing twice.
    controller.stageSelected();
    assert(controller.stagedCount() == offered - 1);
    LibraryEditSession *session = sessionOf(stick);
    const QStringList pending = session->pendingDescriptions();
    assert(pending.size() == offered - 1 && ridersIn(pending) == offered - 1);

    const QVariantMap summary = waitForSyncSave(session, controller, false);
    // Synced tracks, in the page's own words.
    assert(summary.value("written").toInt() == offered - 1 && summary.value("total").toInt() == offered - 1);
    assert(summary.value("skipped").toInt() == 0);
    assert(summary.value("unit").toString() == QStringLiteral("tracks"));
    assert(summary.value("verb").toString() == QStringLiteral("synchronised"));
    // B11 is staged with what its row said, its waveform: its locked grid
    // is not asked for, so not "left alone" either.
    assert(summary.value("notes").toString()
           == QStringLiteral("From rekordbox, 9 beat grids and 10 waveforms copied, 9 tracks now load without "
                             "analysis. Engine's own analysis of one track was kept on the stick first."));
    // The written rows leave; B04's, unticked, stays.
    assert(controller.planCount() == 1 && controller.stagedCount() == 0);
    assert(analysisOnlyOf(controller, stick, "B04") == 0);

    const auto after = analysisRows(stick.db);
    const std::int64_t b03Id = trackIdOf(stick, "B03");
    const std::int64_t b04Id = trackIdOf(stick, "B04");
    const std::int64_t b11Id = trackIdOf(stick, "B11");
    {
        const auto &row = after.at(b03Id);
        assert(row[IsAnalyzed] == "1" && row[TrackData] != "<null>" && row[BeatData] != "<null>"
               && row[Overview] != "<null>" && "B03 completed");
    }
    assert(after.at(b04Id) == in.before.at(b04Id) && "B04, unticked, as it was");
    assert(after.at(b11Id)[BeatData] == in.before.at(b11Id)[BeatData] && "B11's locked grid left alone");
    assert(after.at(b11Id)[Overview] != in.before.at(b11Id)[Overview] && "B11's waveform copied");
    // Every row not staged as it was.
    std::set<std::int64_t> changedIds;
    for (const auto &[id, row] : in.before) {
        if (row != after.at(id)) {
            changedIds.insert(id);
        }
    }
    assert(static_cast<int>(changedIds.size()) == offered - 1 && "exactly the staged rows changed");

    // Read again: B04 is all that is left, the cues still in step.
    analyze(controller, stick);
    printPlans(controller);
    assert(controller.planCount() == 1 && analysisOnlyOf(controller, stick, "B04") == 0);
    std::cout << "cue sync analysis only: " << offered
              << " row(s) with cues in step, the search narrows them, B04 unticked left alone, B03 completed, B11's "
                 "locked grid left alone OK\n";

    // Undo: m.db, overview files, kept analyses and ledger as before.
    waitForSyncSave(session, controller, true);
    {
        testing::DatabaseDump now = testing::dumpDatabase(stick.db);
        testing::DatabaseDump before = in.dumpBefore;
        before.erase("Information");
        now.erase("Information");
        const auto diffs = testing::differences(before, now);
        testing::printDifferences(diffs);
        assert(diffs.empty());
    }
    assert(filesUnder(overviewDirOf(stick)).empty());
    assert(filesUnder(keptDirOf(stick)).empty());
    assert(!fs::exists(seabass::infrastructure::paths::stickBeatGridLedger(stick.root)));
    // And listed again.
    analyze(controller, stick);
    assert(controller.analysisOnlyCount() == offered && controller.planCount() == offered);
    std::cout << "cue sync analysis only undo: the stick as before, the rows listed again OK\n";
}

// Grids only, B11's grid locked: B11 has nothing to offer, and B03 (not
// analysed) is not completed with one part. Both off, and both on with
// Experimental off: no row at all, nothing staged.
void testCueSyncAnalysisOnlyNotOffered()
{
    {
        InStepStick in = makeInStepStick("t73_cuesync_analysis_only_grids", "B03");
        SyncAnalysisPolicy::reset();
        SyncAnalysisPolicy::set(true, false);
        SyncAnalysisPolicy::setExperimentalEnabled(true);
        SyncController controller;
        analyze(controller, in.stick);
        printPlans(controller);
        assert(cueRowCount(controller) == 0 && controller.analysisOnlyCount() > 0);
        assert(analysisOnlyOf(controller, in.stick, "B11") < 0 && "a locked grid is never offered");
        assert(analysisOnlyOf(controller, in.stick, "B03") < 0 && "a row is completed with both parts or not at all");
        for (int i = 0; i < controller.planCount(); ++i) {
            const auto parts = controller.plansModel()->analysis(i);
            assert(parts.grid && !parts.waveform);
            assert(analysisTextOf(controller, i) == QStringLiteral("Beat grid from rekordbox"));
        }
        std::cout << "cue sync analysis only, grids: B11 locked and B03 unanalysed not offered OK\n";
    }
    for (const auto &[name, options, experimental] :
         {std::tuple{"t73_cuesync_analysis_only_never", std::optional<std::pair<bool, bool>>{}, true},
          std::tuple{"t73_cuesync_analysis_only_off", std::optional<std::pair<bool, bool>>{std::make_pair(false, false)},
                     true},
          std::tuple{"t73_cuesync_analysis_only_gated", std::optional<std::pair<bool, bool>>{std::make_pair(true, true)},
                     false}}) {
        InStepStick in = makeInStepStick(name, "B03");
        SyncAnalysisPolicy::reset();
        if (options) {
            SyncAnalysisPolicy::set(options->first, options->second);
        }
        SyncAnalysisPolicy::setExperimentalEnabled(experimental);
        SyncController controller;
        analyze(controller, in.stick);
        assert(controller.planCount() == 0 && controller.analysisOnlyCount() == 0);
        controller.stageSelected();
        assert(controller.stagedCount() == 0 && controller.statusMessage().isEmpty());
        assert(sessionOf(in.stick)->pendingDescriptions().isEmpty());
        std::cout << "cue sync analysis only, " << name << ": no row, nothing staged OK\n";
    }
}

// Staged on the preview's word: a track whose analysis in rekordbox
// changed between the preview and the save (here: another track's analysis
// files put in its place) is left alone, a skip, never written with what
// the preview did not show; the others land.
void testCueSyncAnalysisOnlyChangedSincePreview()
{
    InStepStick in = makeInStepStick("t73_cuesync_analysis_only_changed", "B03");
    const auto &stick = in.stick;
    SyncAnalysisPolicy::reset();
    SyncAnalysisPolicy::set(true, true);
    SyncAnalysisPolicy::setExperimentalEnabled(true);
    SyncController controller;
    analyze(controller, stick);
    const int offered = controller.analysisOnlyCount();
    assert(offered > 1 && analysisOnlyOf(controller, stick, "B03") >= 0);
    controller.stageSelected();
    assert(controller.stagedCount() == offered);
    // B04's analysis files put where B03's are.
    const fs::path b03 = anlzDatOf(stick.root, "B03");
    const fs::path b04 = anlzDatOf(stick.root, "B04");
    for (const char *ext : {".DAT", ".EXT", ".2EX"}) {
        const fs::path from = fs::path(b04).replace_extension(ext);
        const fs::path to = fs::path(b03).replace_extension(ext);
        if (fs::exists(from)) {
            // The analysis files are hard links to the committed fixture
            // (copyPioneerFixture): the link goes first, never written
            // through.
            fs::remove(to);
            fs::copy_file(from, to);
        }
    }
    const QVariantMap summary = waitForSyncSave(sessionOf(stick), controller, false);
    assert(summary.value("skipped").toInt() == 1 && summary.value("written").toInt() == offered - 1);
    assert(summary.value("notes").toString().contains(
        QStringLiteral("one track (rekordbox's analysis changed since it was checked)")));
    const auto after = analysisRows(stick.db);
    const std::int64_t b03Id = trackIdOf(stick, "B03");
    assert(after.at(b03Id) == in.before.at(b03Id) && "B03 left as it was");
    std::cout << "cue sync analysis only: a track changed since the preview is skipped, the rest land OK\n";
}

// The switches turned between Analyze and Stage, either way, or
// Experimental turned off: the rows say what the analysis previewed then,
// so nothing is staged ("Re-Analyze to update the list"). Read again, the
// list stages as it says.
void testCueSyncSwitchesMovedBeforeStage()
{
    using Switches = std::pair<bool, bool>;
    const QString moved = QStringLiteral("Copying beat grids and waveforms from rekordbox was changed in Preferences "
                                         "since the list was read. Re-Analyze to update the list.");
    struct Case
    {
        const char *name;
        Switches analysed;
        Switches staged;
        bool experimentalAtStage;
    };
    for (const Case &c : {Case{"t73_cuesync_moved_off", {true, true}, {true, false}, true},
                          Case{"t73_cuesync_moved_on", {true, false}, {true, true}, true},
                          Case{"t73_cuesync_moved_from_none", {false, false}, {true, true}, true},
                          Case{"t73_cuesync_moved_gated", {true, true}, {true, true}, false}}) {
        InStepStick in = makeInStepStick(c.name, "B03");
        makeCueRows(in.stick, CueRowsToEngine, "N3");
        SyncAnalysisPolicy::reset();
        SyncAnalysisPolicy::set(c.analysed.first, c.analysed.second);
        SyncAnalysisPolicy::setExperimentalEnabled(true);
        SyncController controller;
        analyze(controller, in.stick);
        assert(controller.selectedCount() > 0);
        SyncAnalysisPolicy::set(c.staged.first, c.staged.second);
        SyncAnalysisPolicy::setExperimentalEnabled(c.experimentalAtStage);
        controller.stageSelected(false);
        std::cout << "  " << c.name << ": \"" << controller.errorMessage().toStdString() << "\"\n";
        assert(controller.errorMessage() == moved);
        assert(controller.stagedCount() == 0 && sessionOf(in.stick)->pendingDescriptions().isEmpty());
        // Read again with the switches as they are: staged as it says.
        analyze(controller, in.stick);
        controller.stageSelected(false);
        assert(controller.errorMessage().isEmpty() && controller.stagedCount() == controller.planCount());
        for (int i = 0; i < controller.planCount(); ++i) {
            const auto parts = controller.plansModel()->analysis(i);
            const bool on = c.experimentalAtStage;
            assert((!parts.grid || (on && c.staged.first)) && (!parts.waveform || (on && c.staged.second)));
        }
        sessionOf(in.stick)->discard();
        std::cout << "cue sync, switches moved (" << c.name << "): refused, then staged after Re-Analyze OK\n";
    }
}

// Staged with the switches on, turned off (or Experimental off) before
// Save: the analysis is not written, said as a skip, and the cues of a
// cue row still land.
void testCueSyncSwitchOffAfterStage()
{
    {
        InStepStick in = makeInStepStick("t73_cuesync_off_after_stage", "B03");
        SyncAnalysisPolicy::reset();
        SyncAnalysisPolicy::set(true, true);
        SyncAnalysisPolicy::setExperimentalEnabled(true);
        SyncController controller;
        analyze(controller, in.stick);
        const int offered = controller.analysisOnlyCount();
        assert(offered > 0);
        controller.stageSelected(false);
        assert(controller.stagedCount() == offered);
        SyncAnalysisPolicy::set(true, false);
        const QVariantMap summary = waitForSyncSave(sessionOf(in.stick), controller, false);
        std::cout << "  " << summary.value("notes").toString().toStdString() << "\n";
        assert(summary.value("written").toInt() == 0 && summary.value("skipped").toInt() == offered);
        assert(summary.value("notes").toString().contains(
            QStringLiteral("(copying it from rekordbox was turned off in Preferences since it was staged)")));
        assert(analysisRows(in.stick.db) == in.before && "no analysis written");
        assert(filesUnder(overviewDirOf(in.stick)).empty());
        std::cout << "cue sync, a switch turned off after staging: every analysis-only row skipped OK\n";
    }
    {
        SyncController controller;
        CueSyncRun run = runCueSync("t73_cuesync_gated_after_stage", std::make_pair(true, true), true, controller, {},
                                    {}, {}, []() { SyncAnalysisPolicy::setExperimentalEnabled(false); });
        assertCuesLanded(run, CueRowsToEngine);
        assert(run.after == run.before && "Experimental off since staging: no analysis written");
        assert(filesUnder(overviewDirOf(run.stick)).empty() && filesUnder(keptDirOf(run.stick)).empty());
        std::cout << "cue sync, Experimental off after staging: cues land, no analysis OK\n";
    }
}

// The preview could not check a track (here every track: the library's
// uuid cannot name an overview file): its cue row shows no analysis, and
// staged it carries none, so the save lands its cues and writes no
// analysis rather than failing on it. The page says what was not checked.
void testCueSyncPreviewFailedRider()
{
    SyncController controller;
    CueSyncRun run = runCueSync("t73_cuesync_preview_failed", std::make_pair(true, true), true, controller, {}, {},
                                [](const testing::EngineChangeStick &stick) {
                                    const bool done =
                                        testing::sqlExec(stick.db, "UPDATE Information SET uuid = 'not/a/uuid';");
                                    assert(done);
                                    (void)done;
                                });
    for (const auto &[track, text] : run.rowAnalysis) {
        assert(text.isEmpty() && "a track the preview could not check says no analysis");
    }
    const QString unchecked = controller.analysisUncheckedText();
    std::cout << "  \"" << unchecked.toStdString() << "\"\n";
    assert(unchecked.startsWith(QStringLiteral("Beat grids and waveforms could not be checked for "))
           && unchecked.endsWith(QStringLiteral(" tracks.")));
    assert(run.summary.value("error").toString().isEmpty());
    assertCuesLanded(run, CueRowsToEngine);
    assert(run.after == run.before && "no analysis written for a track the preview did not check");
    std::cout << "cue sync, preview failed: no rider, cues land, the page says what was not checked OK\n";
}

// What was kept of B04's own analysis before cannot be read: the save
// would refuse to replace its analysis, so the preview does not offer it,
// and a save of the other rows lands whole instead of rolling back.
void testCueSyncDamagedKeptEntry()
{
    InStepStick in = makeInStepStick("t73_cuesync_damaged_kept", "B03");
    const auto &stick = in.stick;
    SyncAnalysisPolicy::reset();
    SyncAnalysisPolicy::set(true, true);
    SyncAnalysisPolicy::setExperimentalEnabled(true);
    SyncController controller;
    analyze(controller, stick);
    const int b04 = analysisOnlyOf(controller, stick, "B04");
    assert(b04 >= 0 && "precondition: B04 offered while nothing is kept");
    const auto &plan = controller.plansModel()->plans()[static_cast<std::size_t>(b04)];
    const auto item = seabass::application::beatGridCopyItemFor(plan.match.trackA, plan.match.trackB.sourceId,
                                                                pathToUtf8(stick.root));
    assert(!item.ledgerKey.empty());
    const fs::path kept = seabass::infrastructure::local::keptEngineAnalysisFile(stick.root, item.ledgerKey);
    fs::create_directories(kept.parent_path());
    std::ofstream(kept, std::ios::binary) << "seabass-engine-analysis\t3\ndamaged";

    analyze(controller, stick);
    const int offered = controller.analysisOnlyCount();
    assert(analysisOnlyOf(controller, stick, "B04") < 0 && "a track whose kept entry cannot be read is not offered");
    assert(offered > 0);
    controller.stageSelected(false);
    assert(controller.stagedCount() == offered);
    const QVariantMap summary = waitForSyncSave(sessionOf(stick), controller, false);
    assert(summary.value("error").toString().isEmpty() && !summary.value("rolledBack").toBool());
    assert(summary.value("written").toInt() == offered && summary.value("skipped").toInt() == 0);
    const std::int64_t b04Id = trackIdOf(stick, "B04");
    const std::int64_t b03Id = trackIdOf(stick, "B03");
    const auto after = analysisRows(stick.db);
    assert(after.at(b04Id) == in.before.at(b04Id) && "B04 left as it was");
    assert(after.at(b03Id)[IsAnalyzed] == "1" && "the others landed");
    std::cout << "cue sync, a damaged kept entry: its track not offered, the rest land OK\n";
}

void testCueSync()
{
#ifdef SEABASS_HAVE_TAGLIB
    testCueSyncAnalysisOnly();
    testCueSyncAnalysisOnlyNotOffered();
    testCueSyncAnalysisOnlyChangedSincePreview();
    testCueSyncSwitchesMovedBeforeStage();
    testCueSyncSwitchOffAfterStage();
    testCueSyncPreviewFailedRider();
    testCueSyncDamagedKeptEntry();
    testCueSyncBoth();
    testCueSyncUnstage();
    testCueSyncUntick();
    testCueSyncGridsOnly();
    testCueSyncOff();
    SyncAnalysisPolicy::reset();
#else
    std::cerr << "cuesync: needs TagLib (a row is completed from its file's own sample count)\n";
    std::exit(1);
#endif
}

// --- Create Engine Library with rekordbox's analysis -----------------------

using seabass::gui::EngineLibraryCreationTaskResult;

// A stick with only the rekordbox side and its audio: the fixture's PIONEER
// (export 128) and Contents, no Engine Library.
fs::path makeRekordboxOnlyStick(const std::string &name)
{
    const fs::path root = testing::scratchRoot() / name;
    fs::remove_all(root);
    fs::create_directories(root);
    std::error_code ec;
    testing::copyPioneerFixture(g_fixture / "stick" / "PIONEER", root / "PIONEER", ec);
    assert(!ec);
    fs::copy(g_fixture / "stick" / "Contents", root / "Contents", fs::copy_options::recursive, ec);
    assert(!ec);
    return root;
}

// The phases a run names, and a hook on each.
struct PhaseReporter : seabass::application::ProgressReporter
{
    std::function<void(const std::string &)> onPhase;
    std::vector<std::string> phases;
    std::size_t planned = 0;
    void start(const std::string &label, std::size_t total) override
    {
        if (phases.empty()) {
            planned = total;
        }
        phases.push_back(label);
        if (onPhase) {
            onPhase(label);
        }
    }
    void tick(std::size_t) override {}
    void finish() override {}
    void phase(const std::string &label) override
    {
        phases.push_back(label);
        if (onPhase) {
            onPhase(label);
        }
    }
    void warn(const std::string &) override {}
};

EngineLibraryCreationTaskResult createOn(const fs::path &root, bool grids, bool waveforms,
                                         PhaseReporter *reporter = nullptr,
                                         seabass::application::CancellationToken cancel = {})
{
    seabass::application::AnalysisCopyOptions options;
    options.grids = grids;
    options.waveforms = waveforms;
    PhaseReporter own;
    const EngineLibraryCreationTaskResult result = seabass::gui::runEngineLibraryCreation(
        QString::fromStdString(pathToUtf8(root / "PIONEER")), 2, options, reporter ? *reporter : own, cancel);
    std::cout << "  create (" << grids << "," << waveforms << "): " << result.tracksCreated << " of "
              << result.tracksTotal << " created, error \"" << result.errorMessage.toStdString() << "\", note \""
              << result.analysisNote.toStdString() << "\"\n";
    return result;
}

std::string createdDb(const fs::path &root)
{
    return pathToUtf8(root / "Engine Library" / "Database2" / "m.db");
}

// The new library's tables, but what differs between two creations of the
// same library whatever they do: the library's uuid and the clock.
testing::DatabaseDump createdDump(const fs::path &root)
{
    testing::DatabaseDump dump = testing::dumpDatabase(createdDb(root));
    dump.erase("Information");
    dump.erase("Track");
    dump.erase("Playlist");
    dump.erase("PlaylistEntity");
    return dump;
}

// The Track and Playlist rows but the clock, and the playlist entries but
// the library's uuid each names.
std::vector<std::vector<std::string>> createdTracks(const fs::path &root)
{
    auto rows = rowsButClock(createdDb(root), "Track");
    for (auto &row : rowsButClock(createdDb(root), "Playlist")) {
        rows.push_back(std::move(row));
    }
    for (auto &row : testing::sqlRows(createdDb(root), "SELECT id, listId, trackId, nextEntityId, membershipReference "
                                                       "FROM PlaylistEntity ORDER BY id;")) {
        rows.push_back(std::move(row));
    }
    // The library's own uuid (originDatabaseUuid) and a clock written as
    // text ("2026-11-19 10:57:12.15"): another on every creation.
    const auto uuid = testing::sqlRows(createdDb(root), "SELECT uuid FROM Information;").at(0).at(0);
    for (auto &row : rows) {
        for (auto &value : row) {
            if (value == uuid) {
                value = "<library uuid>";
            } else if (value.size() > 19 && value[4] == '-' && value[7] == '-' && value[10] == ' ' && value[13] == ':') {
                value = "<clock>";
            }
        }
    }
    return rows;
}

// The rekordbox track's DeviceLibrary analysis file of the fixture file
// named "T73-<name>-...", from the copy under `root`.
fs::path anlzDatOf(const fs::path &root, const std::string &name)
{
    auto &cache = seabass::gui::LibraryCatalogCache::instance();
    for (const auto &track : cache.tracksFor("rekordbox", pathToUtf8(root / "PIONEER"))) {
        if (track.filename.rfind("T73-" + name + "-", 0) == 0) {
            assert(!track.analysisFile.empty());
            return root / pathFromUtf8(track.analysisFile.substr(1));
        }
    }
    assert(false && "no such track");
    return {};
}

fs::path audioOf(const fs::path &root, const std::string &name)
{
    for (const auto &entry : fs::recursive_directory_iterator(root / "Contents")) {
        if (entry.is_regular_file() && pathToUtf8(entry.path().filename()).rfind("T73-" + name + "-", 0) == 0) {
            return entry.path();
        }
    }
    assert(false && "no such file");
    return {};
}

void testCreateBoth()
{
    const fs::path root = makeRekordboxOnlyStick("t73_create_both");
    PhaseReporter reporter;
    const auto result = createOn(root, true, true, &reporter);
    assert(result.errorMessage.isEmpty() && !result.cancelled && result.tracksCreated > 0);
    // One bar: the plan counts the tracks to give rekordbox's analysis.
    assert(std::find(reporter.phases.begin(), reporter.phases.end(), "Copying rekordbox's analysis")
           != reporter.phases.end());
    const std::string db = createdDb(root);
    const auto rows = analysisRows(db);
    assert(static_cast<int>(rows.size()) == result.tracksCreated);
    const fs::path engine = root / "Engine Library";
    const auto overviews = filesUnder(engine / "Database2" / "OverviewData");
    std::set<std::int64_t> overviewIds;
    for (const auto &[path, bytes] : overviews) {
        overviewIds.insert(std::stoll(pathToUtf8(pathFromUtf8(path).stem())));
    }
    int completed = 0;
    for (const auto &[id, row] : rows) {
        const bool done = row[IsAnalyzed] == "1";
        if (!done) {
            // Left for the player: as the creator always left it.
            assert(row[TrackData] == "<null>" && row[Overview] == "<null>" && row[BeatData] == "<null>");
            assert(!overviewIds.count(id) && "no overview file for a row left for the player");
            continue;
        }
        ++completed;
        assert(row[TrackData] != "<null>" && row[Overview] != "<null>" && row[BeatData] != "<null>");
        assert(overviewIds.count(id) && "the analysed flag never without its overview file");
        const auto file = testing::sqlRows(db, "SELECT path FROM Track WHERE id = " + std::to_string(id) + ";");
        const fs::path audio = fs::weakly_canonical(engine / pathFromUtf8(file.at(0).at(0)));
        const auto exact = seabass::infrastructure::audio::readExactSampleCount(pathToUtf8(audio));
        assert(exact);
        const auto record = djinterop::engine::v3::track_data_blob::from_blob(
            blobOf(db, "SELECT trackData FROM PerformanceData WHERE trackId = " + std::to_string(id) + ";"));
        assert(record.samples == static_cast<std::int64_t>(exact->samples) && "the file's own length");
        assert(record.sample_rate == exact->sampleRateHz);
    }
    std::cout << "  " << completed << " of " << rows.size() << " rows completed, " << overviews.size()
              << " overview file(s)\n";
    assert(completed > 0);
    assert(static_cast<int>(overviews.size()) == completed);
    // The counts are what landed.
    assert(result.analysisCompleted == completed && result.analysisGrids == completed
           && result.analysisWaveforms == completed);
    assert(result.analysisCompleted + result.analysisLeftForPlayer + result.analysisLeftOther == result.tracksCreated);
    std::string ledgerError;
    const auto ledger = seabass::infrastructure::local::readBeatGridLedger(root, &ledgerError);
    assert(ledgerError.empty() && static_cast<int>(ledger.size()) == completed && "the ledger knows every grid");
    assert(result.analysisNote.startsWith(
        QStringLiteral("From rekordbox, %1 beat grids and %1 waveforms copied, %1 tracks now load without analysis.")
            .arg(completed)));
    // The ledger is what a later copy reads: every row it names carries
    // that very grid.
    std::cout << "create both: every row that can be completed is, counts and ledger as landed OK\n";
}

// One track without a readable audio file, one without a preview: left for
// the player and counted, the rest completed, the creation not failed.
void testCreateLeftForPlayer()
{
    const fs::path root = makeRekordboxOnlyStick("t73_create_left");
    // N1: its audio file no longer reads (the bytes of no audio format).
    const fs::path audio = audioOf(root, "N1");
    const auto size = fs::file_size(audio);
    {
        std::ofstream out(audio, std::ios::binary | std::ios::trunc);
        out << std::string(static_cast<std::size_t>(size), '\x5a');
    }
    // N2: no preview, its .EXT and .2EX gone.
    const fs::path dat = anlzDatOf(root, "N2");
    fs::remove(fs::path(dat).replace_extension(".EXT"));
    fs::remove(fs::path(dat).replace_extension(".2EX"));
    const auto result = createOn(root, true, true);
    assert(result.errorMessage.isEmpty() && !result.cancelled);
    const std::string db = createdDb(root);
    for (const char *name : {"N1", "N2"}) {
        const auto row = analysisRows(db).at(trackIdOfDb(db, name));
        assert(row[IsAnalyzed] == "0" && row[BeatData] == "<null>" && row[TrackData] == "<null>");
    }
    assert(result.analysisLeftForPlayer >= 2);
    assert(result.analysisNote.contains(QStringLiteral("no length for the file")));
    assert(result.analysisNote.contains(QStringLiteral("no rekordbox waveform")));
    std::cout << "create left for the player: an unreadable file and a missing preview counted with why OK\n";
}

// Off, and one switch alone, make the library the creator makes without
// any of this (the infrastructure call as it always was).
void testCreateOffAndOneSwitch()
{
    const fs::path plain = makeRekordboxOnlyStick("t73_create_plain");
    {
        auto &cache = seabass::gui::LibraryCatalogCache::instance();
        const auto tracks = cache.tracksFor("rekordbox", pathToUtf8(plain / "PIONEER"));
        // The rates as Create Engine Library hands them in.
        seabass::infrastructure::audio::TagLibMetadataProbe probe;
        seabass::infrastructure::local::CachedSampleRates rates(
            seabass::infrastructure::paths::stickRootForCatalogPath(pathToUtf8(plain / "PIONEER")), probe);
        const auto made = seabass::infrastructure::engine::EngineLibraryCreator::create(
            pathToUtf8(plain / "Engine Library"), tracks, seabass::infrastructure::engine::EngineSchemaGeneration::V3,
            seabass::application::NullProgressReporter::instance(), seabass::application::CancellationToken::none(),
            seabass::infrastructure::engine::readRekordboxImportState({}, pathToUtf8(plain / "PIONEER")).librarySequence,
            [&rates](const std::string &file) { return rates.rateOf(file); });
        assert(made.errorMessage.empty());
    }
    const auto plainDump = createdDump(plain);
    const auto plainTracks = createdTracks(plain);
    for (const auto &[grids, waveforms] : {std::pair{false, false}, std::pair{true, false}, std::pair{false, true}}) {
        const fs::path root = makeRekordboxOnlyStick("t73_create_" + std::to_string(grids) + std::to_string(waveforms));
        const auto result = createOn(root, grids, waveforms);
        assert(result.errorMessage.isEmpty());
        const auto diffs = testing::differences(plainDump, createdDump(root));
        testing::printDifferences(diffs);
        assert(diffs.empty() && "the same library as the creator always made");
        assert(createdTracks(root) == plainTracks);
        assert(filesUnder(root / "Engine Library" / "Database2" / "OverviewData").empty());
        assert(!fs::exists(seabass::infrastructure::paths::stickBeatGridLedger(root)));
        if (!grids && !waveforms) {
            assert(result.analysisNote.isEmpty());
        } else {
            assert(result.analysisLeftForPlayer == result.tracksCreated);
            assert(result.analysisNote
                   == QStringLiteral("Analysis left alone: %1 tracks left for the player to analyse (a new track is "
                                     "completed only with both beat grids and waveforms switched on).")
                          .arg(result.tracksCreated));
        }
    }
    std::cout << "create off and one switch: the library the creator always made, nothing else OK\n";
}

std::vector<fs::path> scratchBuilds()
{
    std::vector<fs::path> out;
    for (const auto &entry : fs::directory_iterator(fs::temp_directory_path())) {
        if (pathToUtf8(entry.path().filename()).rfind("seabass-engine-build-", 0) == 0) {
            out.push_back(entry.path());
        }
    }
    return out;
}

// A cancel during the copy of rekordbox's analysis, and a write that fails
// there: the stick as it was, no half-made library, no ledger, no scratch.
void testCreateCancelAndFailure()
{
    {
        const fs::path root = makeRekordboxOnlyStick("t73_create_cancel");
        seabass::application::CancellationToken cancel;
        PhaseReporter reporter;
        reporter.onPhase = [&](const std::string &label) {
            if (label == "Copying rekordbox's analysis") {
                cancel.cancel();
            }
        };
        const auto result = createOn(root, true, true, &reporter, cancel);
        assert(result.cancelled && result.errorMessage.isEmpty());
        assert(!fs::exists(root / "Engine Library") && "nothing created on the stick");
        assert(!fs::exists(seabass::infrastructure::paths::stickBeatGridLedger(root)));
        assert(scratchBuilds().empty() && "the scratch library is gone");
    }
    {
        const fs::path root = makeRekordboxOnlyStick("t73_create_failure");
        PhaseReporter reporter;
        reporter.onPhase = [&](const std::string &label) {
            if (label != "Copying rekordbox's analysis") {
                return;
            }
            // The overview folder made a file: the first overview write fails.
            const auto builds = scratchBuilds();
            assert(builds.size() == 1);
            std::ofstream(builds.front() / "Database2" / "OverviewData") << "x";
        };
        const auto result = createOn(root, true, true, &reporter);
        assert(!result.errorMessage.isEmpty() && "a write that fails fails the creation");
        assert(!fs::exists(root / "Engine Library") && "nothing created on the stick");
        assert(!fs::exists(seabass::infrastructure::paths::stickBeatGridLedger(root)));
        assert(scratchBuilds().empty());
    }
    std::cout << "create cancel and failure: no library, no ledger, no scratch left OK\n";
}

void testCreate()
{
#ifdef SEABASS_HAVE_TAGLIB
    // The scratch builds in a folder of this test's own, to look into.
    const fs::path tmp = testing::scratchRoot() / "testshark_t73_create" / "tmp";
    fs::create_directories(tmp);
    qputenv("TMPDIR", QByteArray::fromStdString(pathToUtf8(tmp)));
    testCreateBoth();
    testCreateLeftForPlayer();
    testCreateOffAndOneSwitch();
    testCreateCancelAndFailure();
#else
    std::cerr << "create: needs TagLib (a row is completed from its file's own sample count)\n";
    std::exit(1);
#endif
}

}  // namespace

int main(int argc, char **argv)
{
    seabass::tests::assumeNothingElseRunning();
    if (argc < 3) {
        std::cerr << "usage: testshark_t73_test <tests/fixtures/testshark_t73> fixture|proposal|save|insync|replay|analysis\n";
        return 2;
    }
    qputenv("SEABASS_IGNORE_REMOVABLE_MEDIA", "1");
    // Every line out before an assert ends the process.
    std::cout << std::unitbuf;
    g_fixture = pathFromUtf8(argv[1]);
    const std::string which = argv[2];
    const fs::path scratch = testing::scratchRoot() / ("testshark_t73_" + which);
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    testing::sandboxSeabassHome(scratch / "home");
    testing::sandboxSettings(scratch / "config");
    QCoreApplication app(argc, argv);

    if (which == "fixture") {
        testFixture();
    } else if (which == "proposal") {
        testProposal();
    } else if (which == "save") {
        testSaveUndoSave();
    } else if (which == "insync") {
        testInSync();
    } else if (which == "replay") {
        testReplay();
    } else if (which == "analysis") {
        testAnalysis();
    } else if (which == "create") {
        testCreate();
    } else if (which == "cuesync") {
        testCueSync();
    } else {
        std::cerr << "unknown case " << which << "\n";
        return 2;
    }
    std::cout << "testshark_t73_test " << which << ": OK\n";
    return 0;
}
