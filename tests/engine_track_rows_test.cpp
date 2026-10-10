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
//
// createEngineTrack adds a row to an existing library, over a second copy
// of the fixture with two WAV files written beside it (the fixture has no
// audio), one at 48 kHz and one at 44.1 kHz, both read by TagLib for their
// rate:
//
// - the row reads back through LibdjinteropEngineReader with the title,
//   artist, BPM, key, duration, rating, comment and path it was given;
// - it alone is left for the player to analyse (isAnalyzed 0, NULL
//   trackData, overviewWaveFormData and beatData); the 350 analysed rows
//   of the fixture stay 350 (the whole-library UPDATE the creator runs
//   would make that 0: this is the red check for the per-id marking);
// - its cues sit at the file's own sample offsets: Engine keeps a cue as
//   a sample offset (quickCues, loops, the main cue), so 1000 ms is 48000
//   samples in a 48 kHz file and 44100 in a 44.1 kHz one. A row waiting
//   for analysis has no trackData and so records no rate. A reader with
//   no sample rate source guesses 44.1 kHz, and the 48 kHz row's cues
//   read back 48000/44100 times late; given a source that asks the file
//   (TagLib), both rows read back at the times they were written, through
//   readAll() and through a source-less read corrected by fillCues() as
//   the catalog cache's Cues stage does;
// - pdbImportKey 0, dateAdded the time of the call, the Information row
//   single and byte for byte as before;
// - a path the library already has, a write root with no library, a file
//   that is not there and a missing sample rate are refused, adding
//   nothing; markForDeviceAnalysis refuses an unknown or repeated id and
//   rolls back;
// - a library whose one Information row is at id 2 takes a row and keeps
//   its Information row at 2; one with two Information rows is refused.

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
#include "infrastructure/engine/engine_track_rows.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#if defined(SEABASS_HAVE_TAGLIB)
#include "infrastructure/audio/taglib_metadata_probe.hpp"
#endif
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

// A PCM WAV of silence: 16-bit stereo, `seconds` long, at `rate` Hz.
void writeWav(const fs::path &file, std::uint32_t rate, std::uint32_t seconds)
{
    const std::uint16_t channels = 2;
    const std::uint16_t bits = 16;
    const std::uint32_t byteRate = rate * channels * bits / 8;
    const std::uint32_t dataBytes = byteRate * seconds;
    std::ofstream out(file, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.put(static_cast<char>((v >> (8 * i)) & 0xff));
        }
    };
    const auto u16 = [&](std::uint16_t v) {
        out.put(static_cast<char>(v & 0xff));
        out.put(static_cast<char>(v >> 8));
    };
    out.write("RIFF", 4);
    u32(36 + dataBytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);  // PCM
    u16(channels);
    u32(rate);
    u32(byteRate);
    u16(static_cast<std::uint16_t>(channels * bits / 8));
    u16(bits);
    out.write("data", 4);
    u32(dataBytes);
    const std::string silence(dataBytes, '\0');
    out.write(silence.data(), static_cast<std::streamsize>(silence.size()));
    assert(out.good());
}

// A 1x1 PNG, the smallest image a cover check takes for one.
void writePng(const fs::path &file)
{
    static const unsigned char Png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
        0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d,
        0xb4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char *>(Png), sizeof(Png));
    assert(out.good());
}

// The file's own rate, through TagLib when the build has it.
double probedRate(const fs::path &file, double written)
{
#if defined(SEABASS_HAVE_TAGLIB)
    infrastructure::audio::TagLibMetadataProbe probe;
    const auto meta = probe.read(pathToUtf8(file));
    assert(meta.has_value() && "TagLib reads the WAV");
    assert(meta->sampleRate == static_cast<int>(written) && "and its rate is the one written");
    return meta->sampleRate;
#else
    std::cout << "(built without TagLib: using the rate written into " << pathToUtf8(file.filename()) << ")\n";
    return written;
#endif
}

bool near(double a, double b)
{
    return std::fabs(a - b) < 1e-6;
}

std::optional<std::string> text(const std::string &file, const std::string &sql)
{
    sqlite3 *handle = nullptr;
    const int opened = sqlite3_open_v2(file.c_str(), &handle, SQLITE_OPEN_READONLY, nullptr);
    assert(opened == SQLITE_OK);
    sqlite3_stmt *stmt = nullptr;
    const int prepared = sqlite3_prepare_v2(handle, sql.c_str(), -1, &stmt, nullptr);
    assert(prepared == SQLITE_OK);
    std::optional<std::string> out;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        out = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);
    return out;
}

const domain::Track *byId(const std::vector<domain::Track> &tracks, std::int64_t id)
{
    for (const auto &t : tracks) {
        if (t.sourceId == std::to_string(id)) {
            return &t;
        }
    }
    return nullptr;
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

    // 8. createEngineTrack, over a second copy of the fixture with audio
    //    files beside it, as on a stick: <root>/Engine Library and
    //    <root>/Music.
    {
        const fs::path root = scratch / "create";
        const fs::path lib = root / "Engine Library";
        fs::create_directories(root / "Music");
        fs::copy(fixture / "engine", lib, fs::copy_options::recursive);
        const std::string libUtf8 = pathToUtf8(lib);
        const std::string cdb = pathToUtf8(lib / "Database2" / "m.db");
        const fs::path wav48 = root / "Music" / "Forty Eight.wav";
        const fs::path wav44 = root / "Music" / pathFromUtf8("Caf\xc3\xa9 \xc3\x84.wav");
        writeWav(wav48, 48000, 4);
        writeWav(wav44, 44100, 4);
        const fs::path png = root / "Music" / "cover.png";
        writePng(png);
        const double rate48 = probedRate(wav48, 48000.0);
        const double rate44 = probedRate(wav44, 44100.0);

        // The fixture as committed: 1564 tracks, 350 of them analysed, one
        // Information row at id 1.
        assert(scalar(cdb, "SELECT count(*) FROM Track;") == 1564);
        assert(scalar(cdb, "SELECT count(*) FROM Track WHERE isAnalyzed = 1;") == 350);
        assert(scalar(cdb, "SELECT count(*) FROM Track WHERE isAnalyzed = 0;") == 1214);
        assert(scalar(cdb, "SELECT count(*) FROM Information;") == 1 && scalar(cdb, "SELECT id FROM Information;") == 1);
        std::string err;
        const auto infoBefore = readEngineInformation(cdb, &err);
        assert(infoBefore && err.empty() && infoBefore->ids == std::vector<std::int64_t>{1});

        // Hot cue on pad 1 at 1 s, a hot loop on pad 2 from 1.5 s to 2 s,
        // a memory cue at 3 s (Engine: the first free pad, 3, and the main
        // cue).
        const auto makeTrack = [&](const fs::path &file, const std::string &title, double rate) {
            NewEngineTrack t;
            t.source.title = title;
            t.source.artist = "Art\xc3\xafst";
            t.source.bpm = 126.0;
            t.source.key = "Fm";
            t.source.durationSeconds = 4.0;
            t.source.bitrate = 1411;
            t.source.rating = 4;
            t.source.comment = "made by hand";
            t.source.filePath = pathToUtf8(file);
            std::error_code sizeEc;
            const auto size = fs::file_size(file, sizeEc);
            t.source.fileSizeBytes = sizeEc ? 0 : size;
            domain::CuePoint hot{domain::CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "one"};
            domain::CuePoint loop{domain::CuePoint::Kind::Hot, 2, 1500.0, "#00FF00", "loop"};
            loop.isLoop = true;
            loop.loopEndMs = 2000.0;
            domain::CuePoint memory{domain::CuePoint::Kind::Memory, 0, 3000.0, "", ""};
            t.source.cues = {hot, loop, memory};
            t.sampleRateHz = rate;
            t.realEngineLibraryPath = libUtf8;
            return t;
        };

        const auto callStart = std::chrono::system_clock::now();
        const auto secondsNow = [](std::chrono::system_clock::time_point at) {
            return std::chrono::duration_cast<std::chrono::seconds>(at.time_since_epoch()).count();
        };
        EngineTrackCover cover;
        err = "stale";
        const std::int64_t id48 = createEngineTrack(libUtf8, makeTrack(wav48, "Forty Eight", rate48), &cover, &err);
        if (id48 < 0) {
            std::cerr << "createEngineTrack: " << err << "\n";
        }
        assert(id48 == 1575 && err.empty() && "the next id after the fixture's 1574");
        assert(!cover.written && cover.problem.empty() && cover.filesWritten.empty() && "no cover asked for");

        std::vector<std::string> protectedFiles;
        EngineTrackCover cover44;
        cover44.beforeWrite = [&](const std::string &f) { protectedFiles.push_back(f); };
        NewEngineTrack t44 = makeTrack(wav44, "Caf\xc3\xa9", rate44);
        t44.source.artworkPath = pathToUtf8(png);
        const std::int64_t id44 = createEngineTrack(libUtf8, t44, &cover44, &err);
        if (id44 < 0) {
            std::cerr << "createEngineTrack: " << err << "\n";
        }
        assert(id44 == 1576 && err.empty());
        const auto callEnd = std::chrono::system_clock::now();
        if (!cover44.written) {
            std::cerr << "cover: " << cover44.problem << "\n";
        }
        assert(cover44.written && cover44.filesWritten.size() == 1 && protectedFiles == cover44.filesWritten
               && "the cover went in, its one file announced before it was written");
        assert(fs::exists(pathFromUtf8(cover44.filesWritten[0])));
        assert(scalar(cdb, "SELECT count(*) FROM Track;") == 1566);
        std::cout << "case 8 (two rows created, ids 1575 and 1576, the second with its cover) OK\n";

        // 9. The rows as libdjinterop wrote them and Engine's triggers
        //    finished them.
        for (const std::int64_t id : {id48, id44}) {
            const std::string where = " FROM Track WHERE id = " + std::to_string(id) + ";";
            assert(scalar(cdb, "SELECT pdbImportKey" + where) == 0 && "no player import is faked");
            assert(scalar(cdb, "SELECT isAnalyzed" + where) == 0);
            assert(scalar(cdb, "SELECT originTrackId" + where) == id && "Engine's trigger gave it its own id");
            assert(text(cdb, "SELECT originDatabaseUuid" + where) == std::optional<std::string>(
                       "20e9f3a8-b5e1-424c-bed3-95cfcbab6655"));
            assert(scalar(cdb, "SELECT dateCreated" + where) == 0);
            assert(scalar(cdb, "SELECT isMetadataImported" + where) == 1 && "as libdjinterop writes it");
            const std::int64_t added = scalar(cdb, "SELECT dateAdded" + where);
            assert(added >= secondsNow(callStart) && added <= secondsNow(callEnd) + 1 && "dateAdded is the call");
            const auto blobs = rows(cdb, "SELECT trackData, overviewWaveFormData, beatData, quickCues IS NOT NULL, "
                                         "loops IS NOT NULL FROM PerformanceData WHERE trackId = "
                                         + std::to_string(id) + ";");
            assert(blobs.size() == 1);
            assert(blobs[0][0] == -1 && blobs[0][1] == -1 && blobs[0][2] == -1 && "the analysis blobs are NULL");
            assert(blobs[0][3] == 1 && blobs[0][4] == 1 && "the cues and loops are kept");
        }
        assert(text(cdb, "SELECT path FROM Track WHERE id = " + std::to_string(id48) + ";")
               == std::optional<std::string>("../Music/Forty Eight.wav"));
        assert(text(cdb, "SELECT path FROM Track WHERE id = " + std::to_string(id44) + ";")
               == std::optional<std::string>("../Music/Caf\xc3\xa9 \xc3\x84.wav"));
        assert(text(cdb, "SELECT filename FROM Track WHERE id = " + std::to_string(id44) + ";")
               == std::optional<std::string>("Caf\xc3\xa9 \xc3\x84.wav"));
        // Only these two: the 350 the player analysed are still analysed.
        // A library-wide "UPDATE Track SET isAnalyzed = 0" leaves 0 here.
        assert(scalar(cdb, "SELECT count(*) FROM Track WHERE isAnalyzed = 1;") == 350);
        assert(scalar(cdb, "SELECT count(*) FROM Track WHERE isAnalyzed = 0;") == 1216);
        assert(scalar(cdb, "SELECT count(*) FROM PerformanceData WHERE trackData IS NOT NULL;") == 1566 - 1213
               && "every analysed blob of the fixture is still there");
        const auto infoAfter = readEngineInformation(cdb, &err);
        assert(infoAfter && infoAfter->ids == infoBefore->ids && infoAfter->values == infoBefore->values
               && "the Information row, single and unchanged");
        std::cout << "case 9 (pdbImportKey 0, dateAdded now, unanalysed alone, 350 analysed kept, Information unchanged) OK\n";

        // 10. Read back as Seabass reads a stick.
        LibdjinteropEngineReader reader(libUtf8);
        const auto tracks = reader.readAll();
        assert(tracks.size() == 1566);
        for (const std::int64_t id : {id48, id44}) {
            const domain::Track *t = byId(tracks, id);
            assert(t != nullptr);
            assert(t->artist == "Art\xc3\xafst");
            assert(t->bpm == 126.0);
            assert(t->key == "Fm");
            assert(t->durationSeconds == 4.0);
            assert(t->rating == std::optional<int>(4));
            assert(t->comment == "made by hand");
        }
        assert(byId(tracks, id48)->title == "Forty Eight");
        assert(byId(tracks, id44)->title == "Caf\xc3\xa9");
        assert(byId(tracks, id48)->filePath == pathToUtf8(wav48));
        assert(byId(tracks, id44)->filePath == pathToUtf8(wav44));
        std::cout << "case 10 (title, artist, BPM, key, duration, rating, comment, path read back) OK\n";

        // 11. The cues, as sample offsets: what the player reads. Engine
        //     keeps them at the file's own rate, which is the assumption
        //     the whole row rests on.
        {
            auto engine = djinterop::engine::load_database(libUtf8);
            const auto check = [&](std::int64_t id, double pad1, double loopIn, double loopOut, double pad3) {
                auto t = engine.track_by_id(id);
                assert(t);
                const auto hot = t->hot_cues();
                assert(hot.size() == 8);
                assert(hot[0] && hot[0]->sample_offset == pad1 && hot[0]->label == "one");
                assert(!hot[1] && "pad 2 is a loop, not a cue");
                assert(hot[2] && hot[2]->sample_offset == pad3 && "the memory cue took the first free pad");
                for (size_t i = 3; i < 8; ++i) {
                    assert(!hot[i]);
                }
                const auto loops = t->loops();
                assert(loops[1] && loops[1]->start_sample_offset == loopIn && loops[1]->end_sample_offset == loopOut);
                assert(t->main_cue() == std::optional<double>(pad3) && "the memory cue is the main cue too");
            };
            check(id48, 48000.0, 72000.0, 96000.0, 144000.0);
            check(id44, 44100.0, 66150.0, 88200.0, 132300.0);
        }
        // Through a reader with no sample rate source: right at 44.1 kHz.
        // At 48 kHz the row records no rate (no trackData until the player
        // analyses), so the reader takes its 44.1 kHz guess and every cue
        // reads 48000/44100 late: 1000 ms reads 1088.435... ms.
        const auto cueAt = [](const domain::Track &t, bool loop, int pad) -> const domain::CuePoint * {
            for (const auto &c : t.cues) {
                if (c.kind == domain::CuePoint::Kind::Hot && c.hotCueNumber == pad && c.isLoop == loop) {
                    return &c;
                }
            }
            return nullptr;
        };
        {
            const domain::Track &t = *byId(tracks, id44);
            assert(cueAt(t, false, 1) && near(cueAt(t, false, 1)->positionMs, 1000.0));
            assert(cueAt(t, true, 2) && near(cueAt(t, true, 2)->positionMs, 1500.0)
                   && near(cueAt(t, true, 2)->loopEndMs, 2000.0));
            assert(cueAt(t, false, 3) && near(cueAt(t, false, 3)->positionMs, 3000.0));
        }
        {
            const domain::Track &t = *byId(tracks, id48);
            assert(cueAt(t, false, 1) && near(cueAt(t, false, 1)->positionMs, 1088.4353741496598));
            assert(cueAt(t, true, 2) && near(cueAt(t, true, 2)->positionMs, 1632.6530612244899)
                   && near(cueAt(t, true, 2)->loopEndMs, 2176.8707482993197));
            assert(cueAt(t, false, 3) && near(cueAt(t, false, 3)->positionMs, 3265.3061224489797));
        }
        std::cout << "case 11 (cues at the file's sample offsets, 48 kHz and 44.1 kHz; with no sample rate source "
                     "the reader's 44.1 kHz guess) OK\n";

        // 11b. With a source that asks the file, as the catalog cache's
        //      Cues stage gives one: both rows read back at the times they
        //      were written. Through readAll(), and through a read with no
        //      source whose cues fillCues() then corrects.
        {
            std::map<std::string, int> asked;
            const LibdjinteropEngineReader::SampleRateSource source =
                [&](const std::string &file) -> std::optional<double> {
                ++asked[file];
#if defined(SEABASS_HAVE_TAGLIB)
                infrastructure::audio::TagLibMetadataProbe probe;
                const auto meta = probe.read(file);
                if (!meta || meta->sampleRate <= 0) {
                    return std::nullopt;
                }
                return static_cast<double>(meta->sampleRate);
#else
                if (file == pathToUtf8(wav48)) {
                    return rate48;
                }
                if (file == pathToUtf8(wav44)) {
                    return rate44;
                }
                return std::nullopt;
#endif
            };
            const auto rightTimes = [&](const std::vector<domain::Track> &read) {
                for (const std::int64_t id : {id48, id44}) {
                    const domain::Track &t = *byId(read, id);
                    // Pad 1, the loop on pad 2, the memory cue on pad 3 and
                    // as the main cue.
                    assert(t.cues.size() == 4);
                    assert(cueAt(t, false, 1) && near(cueAt(t, false, 1)->positionMs, 1000.0));
                    assert(cueAt(t, true, 2) && near(cueAt(t, true, 2)->positionMs, 1500.0)
                           && near(cueAt(t, true, 2)->loopEndMs, 2000.0));
                    assert(cueAt(t, false, 3) && near(cueAt(t, false, 3)->positionMs, 3000.0));
                    const auto memory =
                        std::find_if(t.cues.begin(), t.cues.end(),
                                     [](const domain::CuePoint &c) { return c.kind == domain::CuePoint::Kind::Memory; });
                    assert(memory != t.cues.end() && near(memory->positionMs, 3000.0));
                }
            };

            LibdjinteropEngineReader withSource(libUtf8);
            withSource.setSampleRateSource(source);
            const auto read = withSource.readAll();
            assert(read.size() == 1566);
            rightTimes(read);
            assert(asked[pathToUtf8(wav48)] == 1 && asked[pathToUtf8(wav44)] == 1 && "each new row's file asked once");
            // And the fixture's own: of its 1212 rows with no trackData, the
            // 23 that carry a cue or a loop (ids 17, 73, 79, 100, 259, 281,
            // 320, 322, 324, 343, 382, 383, 462, 548, 618, 640, 654, 684,
            // 696, 704, 740, 751, 854), whose files this copy does not
            // have; none of its 353 rows with a stored rate.
            assert(asked.size() == 25);
            for (const auto &[file, times] : asked) {
                assert(times == 1);
            }

            auto corrected = tracks;  // case 10's read, with no source
            asked.clear();
            withSource.fillCues(corrected);
            rightTimes(corrected);
            assert(asked[pathToUtf8(wav48)] == 1 && asked[pathToUtf8(wav44)] == 1 && asked.size() == 25);
        }
        std::cout << "case 11b (given the file's rate, 1000 ms reads 1000 ms at 48 kHz and at 44.1 kHz, through "
                     "readAll() and fillCues()) OK\n";

        // 12. Refused, adding nothing.
        const auto refused = [&](const std::string &writeRoot, const NewEngineTrack &t, const std::string &expect) {
            EngineTrackCover c;
            std::string e;
            const std::int64_t got = createEngineTrack(writeRoot, t, &c, &e);
            if (got != -1 || e.find(expect) == std::string::npos) {
                std::cerr << "expected a refusal with \"" << expect << "\", got " << got << ": " << e << "\n";
            }
            assert(got == -1 && e.find(expect) != std::string::npos);
            assert(scalar(cdb, "SELECT count(*) FROM Track;") == 1566);
            assert(scalar(cdb, "SELECT count(*) FROM PerformanceData;") == 1568);
        };
        refused(libUtf8, makeTrack(wav48, "Again", rate48), "already names ../Music/Forty Eight.wav");
        refused(pathToUtf8(root / "no such library"), makeTrack(wav48, "Nowhere", rate48), "no Engine 2.x or 3.x database");
        refused(pathToUtf8(png), makeTrack(wav48, "A file", rate48), "no Engine 2.x or 3.x database");
        refused(pathToUtf8(root), makeTrack(wav48, "Not a library", rate48), "no Engine 2.x or 3.x database");
        assert(!fs::exists(root / "Database2") && "and nothing was created there");
        refused(libUtf8, makeTrack(root / "Music" / "gone.wav", "Gone", rate48), "is not there");
        {
            fs::path fresh = root / "Music" / "No Rate.wav";
            writeWav(fresh, 48000, 1);
            refused(libUtf8, makeTrack(fresh, "No rate", 0.0), "no sample rate");
        }
        bool threw = false;
        try {
            std::string e;
            (void)createEngineTrack(libUtf8, makeTrack(wav48, "x", rate48), nullptr, &e);
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        assert(threw && "the cover out-parameter is required");
        std::cout << "case 12 (an existing path, no library, a missing file, no rate: refused, nothing added) OK\n";

        // 13. markForDeviceAnalysis refuses an unknown or repeated id, and
        //     a refused call rolls back the good id in it.
        const std::int64_t analysed = scalar(cdb, "SELECT min(id) FROM Track WHERE isAnalyzed = 1;");
        err.clear();
        assert(markForDeviceAnalysis(cdb, {analysed, 99999}, &err) == -1 && err.find("no track id=99999") != std::string::npos);
        assert(scalar(cdb, "SELECT isAnalyzed FROM Track WHERE id = " + std::to_string(analysed) + ";") == 1);
        assert(scalar(cdb, "SELECT trackData IS NOT NULL FROM PerformanceData WHERE trackId = " + std::to_string(analysed)
                      + ";")
               == 1 && "rolled back: its analysis is still there");
        assert(markForDeviceAnalysis(cdb, {analysed, analysed}, &err) == -1 && !err.empty());
        assert(scalar(cdb, "SELECT count(*) FROM Track WHERE isAnalyzed = 1;") == 350);
        err = "stale";
        assert(markForDeviceAnalysis(cdb, {analysed}, &err) == 1 && err.empty());
        assert(scalar(cdb, "SELECT count(*) FROM Track WHERE isAnalyzed = 1;") == 349 && "that one, and only that one");
        std::cout << "case 13 (markForDeviceAnalysis: unknown and repeated ids refused and rolled back; one id marks one) OK\n";

        // 14. The Information row's id is not assumed: a player's has been
        //     seen at 2, and a library with one row there takes a new
        //     track and keeps its row where it was. Two rows are refused
        //     before anything is written.
        {
            const fs::path lib2 = root / "Engine Library 2";
            fs::copy(fixture / "engine", lib2, fs::copy_options::recursive);
            const std::string lib2Utf8 = pathToUtf8(lib2);
            const std::string db2 = pathToUtf8(lib2 / "Database2" / "m.db");
            const auto exec = [&](const std::string &sql) {
                sqlite3 *h = nullptr;
                assert(sqlite3_open_v2(db2.c_str(), &h, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
                assert(sqlite3_exec(h, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
                sqlite3_close(h);
            };
            exec("UPDATE Information SET id = 2;");
            NewEngineTrack t = makeTrack(wav48, "At two", rate48);
            t.realEngineLibraryPath = lib2Utf8;
            EngineTrackCover c;
            std::string e = "stale";
            const std::int64_t id = createEngineTrack(lib2Utf8, t, &c, &e);
            if (id < 0) {
                std::cerr << "createEngineTrack: " << e << "\n";
            }
            assert(id == 1575 && e.empty());
            assert(scalar(db2, "SELECT count(*) FROM Information;") == 1 && scalar(db2, "SELECT id FROM Information;") == 2);
            exec("INSERT INTO Information (uuid, schemaVersionMajor, schemaVersionMinor, schemaVersionPatch, "
                 "currentPlayedIndiciator, lastRekordBoxLibraryImportReadCounter) "
                 "SELECT uuid, schemaVersionMajor, schemaVersionMinor, schemaVersionPatch, currentPlayedIndiciator, "
                 "lastRekordBoxLibraryImportReadCounter FROM Information;");
            const std::int64_t tracks = scalar(db2, "SELECT count(*) FROM Track;");
            NewEngineTrack u = makeTrack(wav44, "Two rows", rate44);
            u.realEngineLibraryPath = lib2Utf8;
            assert(createEngineTrack(lib2Utf8, u, &c, &e) == -1 && e.find("Engine expects exactly one row") != std::string::npos);
            assert(scalar(db2, "SELECT count(*) FROM Track;") == tracks && "nothing added");
        }
        std::cout << "case 14 (an Information row at id 2 is accepted and stays; two rows are refused) OK\n";

        // 15. Junk is never written onto a stick (domain::isJunkCue): a
        //     rekordbox export cue on pad 1 at 7 ms and a negative "no cue"
        //     sentinel given to createEngineTrack are dropped and counted;
        //     the real hot cue and memory cue go in as before.
        {
            const fs::path lib3 = root / "Engine Library 3";
            fs::copy(fixture / "engine", lib3, fs::copy_options::recursive);
            const std::string lib3Utf8 = pathToUtf8(lib3);
            NewEngineTrack t = makeTrack(wav48, "Junk", rate48);
            t.realEngineLibraryPath = lib3Utf8;
            domain::CuePoint exportCue{domain::CuePoint::Kind::Hot, 1, 7.0, "#FF0000", ""};
            domain::CuePoint sentinel{domain::CuePoint::Kind::Memory, 0, -0.5, "", ""};
            domain::CuePoint real{domain::CuePoint::Kind::Hot, 2, 1000.0, "#00FF00", "two"};
            domain::CuePoint memory{domain::CuePoint::Kind::Memory, 0, 3000.0, "", ""};
            t.source.cues = {exportCue, sentinel, real, memory};
            EngineTrackCover c;
            std::string e = "stale";
            int dropped = -1;
            const std::int64_t id = createEngineTrack(lib3Utf8, t, &c, &e, &dropped);
            if (id < 0) {
                std::cerr << "createEngineTrack: " << e << "\n";
            }
            assert(id == 1575 && e.empty());
            auto engine = djinterop::engine::load_database(lib3Utf8);
            auto row = engine.track_by_id(id);
            assert(row);
            const auto hot = row->hot_cues();
            assert(hot[0] && hot[0]->sample_offset == 144000.0
                   && "pad 1 is free of the 7 ms export cue, so the memory cue took it");
            for (const auto &h : hot) {
                assert(!h || h->sample_offset >= 48000.0);
            }
            assert(hot[1] && hot[1]->sample_offset == 48000.0 && hot[1]->label == "two");
            assert(row->main_cue() == std::optional<double>(144000.0) && "the main cue is the real memory cue, not 0");
            assert(dropped == 2 && "the 7 ms pad 1 and the sentinel, counted");
        }
        std::cout << "case 15 (junk cues given to createEngineTrack are dropped and counted, never written) OK\n";
    }

    std::cout << "engine_track_rows_test: all cases passed\n";
    return 0;
}
