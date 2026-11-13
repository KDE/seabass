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
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <djinterop/djinterop.hpp>

#include "engine_chain_check.hpp"
#include "engine_change_fixture.hpp"
#include "gui/edit/edit_session_registry.hpp"
#include "gui/edit/library_edit_session.hpp"
#include "gui/rekordbox_export_sync_controller.hpp"
#include "gui/rekordbox_export_sync_list_model.hpp"
#ifdef SEABASS_HAVE_TAGLIB
#include "infrastructure/audio/taglib_metadata_probe.hpp"
#endif
#include "infrastructure/paths/seabass_paths.hpp"
#include "scratch_path.hpp"

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
testing::EngineChangeStick makeStick(const std::string &name, const std::string &engineDb,
                                     const std::string &baseline)
{
    testing::EngineChangeStick stick;
    stick.root = testing::scratchRoot() / name;
    fs::remove_all(stick.root);
    fs::create_directories(stick.root);
    stick.pioneer = stick.root / "PIONEER";
    stick.engine = stick.root / "Engine Library";
    std::error_code ec;
    testing::copyPioneerFixture(g_fixture / "stick" / "PIONEER", stick.pioneer, ec);
    assert(!ec && "the fixture's PIONEER copies");
    fs::copy(g_fixture / "stick" / "Contents", stick.root / "Contents", fs::copy_options::recursive, ec);
    assert(!ec && "the fixture's audio copies");
    fs::create_directories(stick.engine / "Database2");
    fs::copy_file(g_fixture / "engine" / engineDb, stick.engine / "Database2" / "m.db", ec);
    assert(!ec && "the Engine state copies");
    const fs::path record = seabass::infrastructure::paths::stickRekordboxBaseline(stick.root);
    fs::create_directories(record.parent_path());
    fs::copy_file(g_fixture / "baseline" / baseline, record, ec);
    assert(!ec && "the record copies");
    stick.db = pathToUtf8(stick.engine / "Database2" / "m.db");
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

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "usage: testshark_t73_test <tests/fixtures/testshark_t73> fixture|proposal|save|replay\n";
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
    } else if (which == "replay") {
        testReplay();
    } else {
        std::cerr << "unknown case " << which << "\n";
        return 2;
    }
    std::cout << "testshark_t73_test " << which << ": OK\n";
    return 0;
}
