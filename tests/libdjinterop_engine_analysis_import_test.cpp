// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// LibdjinteropEngineCueWriter::importAnalysis() puts rekordbox's analysis
// (grid, waveform, key, tempo) into an Engine row. Over scratch copies of
// the committed anonymized fixture's Engine library (schema 3.0.2, written
// by Denon), one copy per case. The fixture has no rekordbox waveform that
// would match it, so the previews are synthetic bytes; what is checked is
// where they go, not how well they fit.
//
// 1. An analysed row: the overview column is exactly the conversion of the
//    three-band preview over the row's own length, the overview file is
//    the second waveform of it (in the uuid folder of that database), the
//    beat data is byte for byte what writeBeatGrid() writes, and the
//    analysis record, the Track row and every other row and every other
//    overview file are as they were. The same call again is AlreadySame
//    for both parts and writes nothing: no edit time, no file time.
// 2. The colour preview stands in when there is no three-band one, over
//    the row's length when the scrolling waveform's count is not known.
// 3. A row the player has not analysed, with everything there, is
//    completed: every column as the player leaves one, read back through
//    the project's readers and libdjinterop's decoders, the record 68 raw
//    bytes. markForDeviceAnalysis() then gives back the row it was.
// 4. Each missing precondition leaves the whole library as it was, with
//    the outcomes saying why, and no hook called.
// 5. A preview gives the real call's outcomes and writes nothing.
// 6. --no-overview-file and --zero-loudness, as options.
// 7. The file sink gets the path and bytes; the hook fires before any
//    byte changes; the flag goes up last, so a sink that throws leaves a
//    row the player still takes for unanalysed.
// 8. Engine 2.18: the same in the Track table; 1.x: UnsupportedSchema.
// 9. A real Denon overview file decodes with the code that encodes ours.
// 10. A grid takes the mark of Engine's rekordbox import off a row; a
//     marked MP3 row with cues whose shift is not known keeps cues and
//     mark, a file that is not an MP3 loses the mark with its cues byte
//     for byte, cues that cannot be read keep cues and mark.
// 11. With a shift, a marked row's cues move onto Engine's timeline with
//     the grid, under the cue writer's rules; an unmarked row's do not.
// 12. writeHotCues() takes the mark off too when its shift is known, on
//     3.x and on 2.18, and leaves it when not; a value
//     other than 1 stays through it and through a grid.
//
// usage: libdjinterop_engine_analysis_import_test <tests/fixtures>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <djinterop/djinterop.hpp>
#include <djinterop/engine/v2/engine_library.hpp>
#include <djinterop/engine/v3/beat_data_blob.hpp>
#include <djinterop/engine/v3/engine_library.hpp>
#include <djinterop/engine/v3/loops_blob.hpp>
#include <djinterop/engine/v3/overview_waveform_data_blob.hpp>
#include <djinterop/engine/v3/quick_cues_blob.hpp>
#include <djinterop/engine/v3/track_data_blob.hpp>
#include <sqlite3.h>
#include <zlib.h>

#include "domain/beat_grid.hpp"
#include "domain/engine_overview.hpp"
#include "domain/matching_policy.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/engine/engine_track_rows.hpp"
#include "infrastructure/engine/libdjinterop_beat_grid_reader.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_waveform_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using namespace seabass::infrastructure::engine;
using Outcome = BeatGridWriteResult::Outcome;
using Waveform = AnalysisImportResult::Waveform;
using BeatData = djinterop::engine::v3::beat_data_blob;
using TrackData = djinterop::engine::v3::track_data_blob;
using Overview = djinterop::engine::v3::overview_waveform_data_blob;
using QuickCuesBlob = djinterop::engine::v3::quick_cues_blob;
using LoopsBlob = djinterop::engine::v3::loops_blob;

namespace
{

fs::path g_fixtures;
int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

constexpr std::int64_t kAnalysed = 6;
constexpr std::int64_t kAnalysedToo = 7;
constexpr std::int64_t kUnanalysed = 1;
constexpr std::int64_t kUnanalysedToo = 2;
const std::string kUuid = "20e9f3a8-b5e1-424c-bed3-95cfcbab6655";

struct Copy
{
    fs::path root;  // the "Engine Library" folder
    std::string mdb;
};

// A copy, never the fixture itself: opening its m.db in place could
// change it.
Copy freshCopy(const std::string &caseName)
{
    const fs::path dir = testing::scratchRoot() / "engine_analysis_import" / pathFromUtf8(caseName);
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path root = dir / "Engine Library";
    fs::copy(g_fixtures / "anonymized_library" / "engine", root, fs::copy_options::recursive);
    return {root, pathToUtf8(root / "Database2" / "m.db")};
}

sqlite3 *open(const std::string &file, int flags)
{
    sqlite3 *handle = nullptr;
    const int opened = sqlite3_open_v2(file.c_str(), &handle, flags, nullptr);
    assert(opened == SQLITE_OK);
    (void)opened;
    return handle;
}

void exec(const std::string &mdb, const std::string &sql)
{
    sqlite3 *handle = open(mdb, SQLITE_OPEN_READWRITE);
    const int done = sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, nullptr);
    assert(done == SQLITE_OK);
    (void)done;
    sqlite3_close(handle);
}

// Every row of a query, each column through quote(), so NULL, REAL and a
// blob's bytes all say what they are.
std::map<std::int64_t, std::vector<std::string>> rows(const std::string &mdb, const std::string &sql)
{
    sqlite3 *handle = open(mdb, SQLITE_OPEN_READONLY);
    sqlite3_stmt *stmt = nullptr;
    const int prepared = sqlite3_prepare_v2(handle, sql.c_str(), -1, &stmt, nullptr);
    assert(prepared == SQLITE_OK);
    (void)prepared;
    std::map<std::int64_t, std::vector<std::string>> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::vector<std::string> values;
        for (int c = 1; c < sqlite3_column_count(stmt); ++c) {
            const auto *text = sqlite3_column_text(stmt, c);
            values.emplace_back(text ? reinterpret_cast<const char *>(text) : "");
        }
        out[sqlite3_column_int64(stmt, 0)] = std::move(values);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);
    return out;
}

std::map<std::int64_t, std::vector<std::string>> performanceRows(const std::string &mdb)
{
    return rows(mdb, "SELECT trackId, quote(trackData), quote(overviewWaveFormData), quote(beatData), quote(quickCues),"
                     " quote(loops), quote(thirdPartySourceId), quote(activeOnLoadLoops) FROM PerformanceData");
}

// The Track row's columns that are not its edit time.
std::map<std::int64_t, std::vector<std::string>> trackRows(const std::string &mdb)
{
    return rows(mdb, "SELECT id, quote(isAnalyzed), quote(bpm), quote(bpmAnalyzed), quote(key), quote(length),"
                     " quote(isBeatGridLocked), quote(path), quote(title), quote(rating) FROM Track");
}

std::string column(const std::string &mdb, const std::string &table, const std::string &idColumn, std::int64_t id,
                   const std::string &name)
{
    const auto all = rows(mdb, "SELECT " + idColumn + ", quote(" + name + ") FROM " + table + " WHERE " + idColumn
                                   + " = " + std::to_string(id));
    return all.empty() ? "<no row>" : all.begin()->second.at(0);
}

std::vector<std::byte> blobOf(const std::string &mdb, const std::string &table, const std::string &idColumn,
                              std::int64_t id, const std::string &name)
{
    sqlite3 *handle = open(mdb, SQLITE_OPEN_READONLY);
    sqlite3_stmt *stmt = nullptr;
    const std::string sql = "SELECT " + name + " FROM " + table + " WHERE " + idColumn + " = " + std::to_string(id);
    sqlite3_prepare_v2(handle, sql.c_str(), -1, &stmt, nullptr);
    std::vector<std::byte> out;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
        const auto *bytes = static_cast<const std::byte *>(sqlite3_column_blob(stmt, 0));
        out.assign(bytes, bytes + sqlite3_column_bytes(stmt, 0));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);
    return out;
}

std::vector<std::byte> perf(const std::string &mdb, std::int64_t id, const std::string &name)
{
    return blobOf(mdb, "PerformanceData", "trackId", id, name);
}

// The bytes a blob column holds once zlib has given them back.
std::vector<unsigned char> inflated(const std::vector<std::byte> &blob)
{
    if (blob.size() < 4) {
        return {};
    }
    const auto *b = reinterpret_cast<const unsigned char *>(blob.data());
    uLongf size = (uLongf(b[0]) << 24) | (uLongf(b[1]) << 16) | (uLongf(b[2]) << 8) | uLongf(b[3]);
    std::vector<unsigned char> out(size);
    if (uncompress(out.data(), &size, b + 4, static_cast<uLong>(blob.size() - 4)) != Z_OK) {
        return {};
    }
    out.resize(size);
    return out;
}

std::string fileBytes(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<std::byte> asBytes(const std::string &text)
{
    std::vector<std::byte> out(text.size());
    std::transform(text.begin(), text.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

// Every file under the library's overview folder, with its bytes.
std::map<std::string, std::string> overviewFiles(const Copy &copy)
{
    std::map<std::string, std::string> out;
    const fs::path dir = copy.root / "Database2" / "OverviewData";
    if (!fs::exists(dir)) {
        return out;
    }
    for (const auto &entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            out[pathToUtf8(fs::relative(entry.path(), copy.root).generic_u8string())] = fileBytes(entry.path());
        }
    }
    return out;
}

LibdjinteropEngineCueWriter unshiftedWriter(const std::string &root)
{
    LibdjinteropEngineCueWriter writer(root);
    writer.setLeadInSource([](const std::string &) { return std::optional<domain::Mp3LeadIn>(); });
    return writer;
}

std::vector<domain::Beat> constantGrid(double firstMs, double beatMs, double untilMs)
{
    std::vector<domain::Beat> beats;
    for (int i = 0; firstMs + i * beatMs < untilMs; ++i) {
        beats.push_back({std::round(firstMs + i * beatMs), i % 4 + 1});
    }
    return beats;
}

// Bytes that look like nothing in particular, the same every run.
std::string synthetic(std::size_t entries, std::size_t entryBytes, unsigned seed)
{
    std::string out(entries * entryBytes, '\0');
    unsigned state = seed;
    for (char &c : out) {
        state = state * 1103515245u + 12345u;
        c = static_cast<char>((state >> 16) & 0x7f);
    }
    return out;
}

// What the writer must have put there: the domain conversion, in
// libdjinterop's blob.
Overview expectedBlob(const domain::EngineOverview &overview, double samples)
{
    Overview blob;
    blob.samples_per_waveform_point = samples / 1024.0;
    for (const auto &p : overview.points) {
        blob.waveform_points.push_back({p.low, p.mid, p.high});
    }
    blob.maximum_point = {overview.finalTriple.low, overview.finalTriple.mid, overview.finalTriple.high};
    return blob;
}

std::string fileOf(std::int64_t id)
{
    return "Database2/OverviewData/" + kUuid + "/" + std::to_string(id) + ".rgb";
}

// Every source beat inside the track read back within a millisecond.
bool readsBack(const std::string &root, std::int64_t id, const std::vector<domain::Beat> &source, double untilMs)
{
    const auto beats = readBeatGrid(root, std::to_string(id));
    std::size_t at = 0;
    for (const domain::Beat &want : source) {
        if (want.timeMs >= untilMs) {
            break;
        }
        while (at < beats.size() && beats[at].timeMs < want.timeMs - 1.0) {
            ++at;
        }
        if (at == beats.size() || std::abs(beats[at].timeMs - want.timeMs) > 1.0 || beats[at].beatInBar != want.beatInBar) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: libdjinterop_engine_analysis_import_test <tests/fixtures>\n";
        return 2;
    }
    g_fixtures = pathFromUtf8(argv[1]);

    // 1. An analysed row, told to write over the waveform the player drew:
    // the waveform replaced, the grid as writeBeatGrid() writes it,
    // everything else as it was; then AlreadySame.
    {
        const Copy copy = freshCopy("analysed");
        const TrackData record = TrackData::from_blob(perf(copy.mdb, kAnalysed, "trackData"));
        check(record.sample_rate > 0.0 && record.samples > 0, "the analysed row has an analysis record");
        const double seconds = static_cast<double>(record.samples) / record.sample_rate;
        const auto scroll = static_cast<std::uint32_t>(std::ceil(seconds * 150.0)) + 7;
        RekordboxAnalysis analysis;
        analysis.beats = constantGrid(37.0, 60000.0 / 121.0, seconds * 1000.0);
        analysis.threeBandPreview = synthetic(1200, 3, 1);
        analysis.colourPreview = synthetic(1200, 6, 2);
        analysis.scrollEntries = scroll;
        analysis.key = 3;
        analysis.bpm = 121.0;

        const auto perfBefore = performanceRows(copy.mdb);
        const auto tracksBefore = trackRows(copy.mdb);
        const auto filesBefore = overviewFiles(copy);
        const auto rawRecordBefore = perf(copy.mdb, kAnalysed, "trackData");

        AnalysisImportOptions replace;
        replace.overwritePlayerWaveforms = true;
        auto writer = unshiftedWriter(pathToUtf8(copy.root));
        const auto result = writer.importAnalysis(std::to_string(kAnalysed), analysis, replace);
        check(result.grid.outcome == Outcome::Written, "an analysed row's grid is written");
        check(result.waveform == Waveform::Written, "and its waveform");
        check(!result.completedRow, "it is not a row completed");
        check(result.overviewFileRelativePath == fileOf(kAnalysed), "the file is in the database's uuid folder: "
                                                                        + result.overviewFileRelativePath);

        const domain::EngineOverview overview = domain::overviewFromThreeBandPreview(
            *analysis.threeBandPreview, scroll / 150.0, seconds);
        const double samples = static_cast<double>(record.samples);
        check(Overview::from_blob(perf(copy.mdb, kAnalysed, "overviewWaveFormData")) == expectedBlob(overview, samples),
              "the overview column is the three-band preview converted over the row's length");
        const std::string file = fileBytes(copy.root / fileOf(kAnalysed));
        check(Overview::from_blob(asBytes(file)) == expectedBlob(domain::secondWaveformFromOverview(overview), samples),
              "the overview file holds the second waveform of it");

        // The grid exactly as writeBeatGrid() leaves it on its own copy.
        const Copy other = freshCopy("analysed-grid-only");
        auto gridWriter = unshiftedWriter(pathToUtf8(other.root));
        const auto gridOnly = gridWriter.writeBeatGrid(std::to_string(kAnalysed), analysis.beats);
        check(perf(copy.mdb, kAnalysed, "beatData") == perf(other.mdb, kAnalysed, "beatData"),
              "the beat data is byte for byte what writeBeatGrid() writes");
        check(gridOnly.gridFingerprint == result.grid.gridFingerprint && gridOnly.markers == result.grid.markers,
              "with its fingerprint and markers");

        check(perf(copy.mdb, kAnalysed, "trackData") == rawRecordBefore, "the analysis record is byte for byte as it was");
        const auto tracksAfter = trackRows(copy.mdb);
        check(tracksAfter == tracksBefore, "every Track row (flag, tempo, key) is as it was");
        auto perfOthersBefore = perfBefore;
        auto perfOthersAfter = performanceRows(copy.mdb);
        perfOthersBefore.erase(kAnalysed);
        perfOthersAfter.erase(kAnalysed);
        check(perfOthersAfter == perfOthersBefore, "no other row's analysis changed");
        auto filesAfter = overviewFiles(copy);
        int otherFilesChanged = 0;
        for (const auto &[name, bytes] : filesAfter) {
            if (name != fileOf(kAnalysed) && (!filesBefore.contains(name) || filesBefore.at(name) != bytes)) {
                ++otherFilesChanged;
            }
        }
        check(otherFilesChanged == 0 && filesAfter.size() >= filesBefore.size(),
              "no other overview file appeared, went or changed");

        // Again: nothing to do, and nothing done.
        exec(copy.mdb, "UPDATE Track SET lastEditTime = 12345 WHERE id = " + std::to_string(kAnalysed));
        const auto fileTime = fs::last_write_time(copy.root / fileOf(kAnalysed));
        const std::string dbBytes = fileBytes(pathFromUtf8(copy.mdb));
        int hookCalls = 0;
        auto again = unshiftedWriter(pathToUtf8(copy.root));
        again.setBeforeBeatGridWrite([&] { ++hookCalls; });
        const auto same = again.importAnalysis(std::to_string(kAnalysed), analysis, replace);
        check(same.grid.outcome == Outcome::AlreadySame && same.waveform == Waveform::AlreadySame,
              "the same call again is AlreadySame for both parts");
        check(same.overviewFileRelativePath.empty(), "and names no file written");
        check(hookCalls == 0, "and calls no hook");
        check(column(copy.mdb, "Track", "id", kAnalysed, "lastEditTime") == "12345", "the row's edit time stays");
        check(fs::last_write_time(copy.root / fileOf(kAnalysed)) == fileTime, "the file's time stays");
        check(fileBytes(pathFromUtf8(copy.mdb)) == dbBytes, "m.db is byte for byte as it was");
        std::cout << "case 1 (analysed row) done\n";
    }

    // 1b. Not told to write over it, the waveform the player drew of an
    // analysed row stays, column and file byte for byte, and is said to
    // (PlayersWaveform), in a preview as in the write; the grid is written
    // all the same. A row the player has not analysed is completed either
    // way, and an analysed row with no overview gets one. A row Seabass
    // completed, both its grids the ledger's, holds Seabass's waveform: a
    // later copy refreshes it; without the ledger's word it is taken for
    // the player's.
    {
        const Copy copy = freshCopy("players-waveform");
        const TrackData record = TrackData::from_blob(perf(copy.mdb, kAnalysed, "trackData"));
        const double seconds = static_cast<double>(record.samples) / record.sample_rate;
        RekordboxAnalysis analysis;
        analysis.beats = constantGrid(37.0, 60000.0 / 121.0, seconds * 1000.0);
        analysis.threeBandPreview = synthetic(1200, 3, 11);
        analysis.scrollEntries = static_cast<std::uint32_t>(std::ceil(seconds * 150.0));
        // A file the player wrote beside the column, which must stay too.
        const fs::path playersFile = copy.root / fileOf(kAnalysed);
        fs::create_directories(playersFile.parent_path());
        std::ofstream(playersFile, std::ios::binary) << synthetic(3000, 1, 12);
        const auto overviewBefore = perf(copy.mdb, kAnalysed, "overviewWaveFormData");
        check(!overviewBefore.empty(), "1b. the analysed row has the player's overview");
        const auto filesBefore = overviewFiles(copy);

        auto writer = unshiftedWriter(pathToUtf8(copy.root));
        AnalysisImportOptions preview;
        preview.previewOnly = true;
        const auto previewed = writer.importAnalysis(std::to_string(kAnalysed), analysis, preview);
        check(previewed.grid.outcome == Outcome::Written && previewed.waveform == Waveform::PlayersWaveform
                  && previewed.overviewFileRelativePath.empty(),
              "1b. a preview says the player's waveform is left alone, the grid written");
        const auto result = writer.importAnalysis(std::to_string(kAnalysed), analysis);
        check(result.grid.outcome == Outcome::Written, "1b. the grid is written");
        check(result.waveform == Waveform::PlayersWaveform, "1b. the player's waveform is left alone, and said to be");
        check(result.overviewFileRelativePath.empty(), "1b. no overview file named");
        check(perf(copy.mdb, kAnalysed, "overviewWaveFormData") == overviewBefore,
              "1b. the overview column is byte for byte the player's");
        check(overviewFiles(copy) == filesBefore, "1b. and every overview file, the player's too");

        // Told to, it is replaced.
        AnalysisImportOptions replace;
        replace.overwritePlayerWaveforms = true;
        const auto replaced = writer.importAnalysis(std::to_string(kAnalysed), analysis, replace);
        check(replaced.waveform == Waveform::Written && replaced.overviewFileRelativePath == fileOf(kAnalysed),
              "1b. with overwritePlayerWaveforms the player's waveform is replaced");
        check(perf(copy.mdb, kAnalysed, "overviewWaveFormData") != overviewBefore
                  && fileBytes(playersFile) != filesBefore.at(fileOf(kAnalysed)),
              "1b. column and file both");

        // An analysed row with no overview gets one, not told to.
        exec(copy.mdb, "UPDATE PerformanceData SET overviewWaveFormData = NULL WHERE trackId = "
                           + std::to_string(kAnalysedToo));
        RekordboxAnalysis colour;
        colour.colourPreview = synthetic(1200, 6, 13);
        AnalysisImportOptions waveformOnly;
        waveformOnly.grids = false;
        const auto none = writer.importAnalysis(std::to_string(kAnalysedToo), colour, waveformOnly);
        check(none.waveform == Waveform::Written && perf(copy.mdb, kAnalysedToo, "overviewWaveFormData").size() > 0,
              "1b. an analysed row with no overview gets rekordbox's");

        // A row the player has not analysed: completed, with or without
        // the option; then Seabass's own waveform, known by the ledger.
        RekordboxAnalysis whole;
        whole.beats = constantGrid(120.0, 480.0, 200000.0);
        whole.threeBandPreview = synthetic(1200, 3, 14);
        whole.scrollEntries = 30000;
        for (const bool over : {false, true}) {
            const Copy fresh = freshCopy(over ? "players-waveform-unanalysed-over" : "players-waveform-unanalysed");
            AnalysisImportOptions options;
            options.fileSampleRateHz = 48000.0;
            options.overwritePlayerWaveforms = over;
            auto w = unshiftedWriter(pathToUtf8(fresh.root));
            const auto completed = w.importAnalysis(std::to_string(kUnanalysed), whole, options);
            check(completed.completedRow && completed.waveform == Waveform::Written,
                  std::string("1b. a row the player has not analysed gets the waveform, ")
                      + (over ? "with" : "without") + " the option");
            if (over) {
                continue;
            }
            RekordboxAnalysis newer = whole;
            newer.threeBandPreview = synthetic(1200, 3, 15);
            const auto seabassBefore = perf(fresh.mdb, kUnanalysed, "overviewWaveFormData");
            AnalysisImportOptions unknown;
            unknown.fileSampleRateHz = 48000.0;
            const auto unknownResult = w.importAnalysis(std::to_string(kUnanalysed), newer, unknown);
            check(unknownResult.waveform == Waveform::PlayersWaveform
                      && perf(fresh.mdb, kUnanalysed, "overviewWaveFormData") == seabassBefore,
                  "1b. without the ledger's word, Seabass's waveform is taken for the player's");
            AnalysisImportOptions ledger = unknown;
            ledger.ownGridFingerprint = completed.grid.gridFingerprint;
            const auto refreshed = w.importAnalysis(std::to_string(kUnanalysed), newer, ledger);
            check(refreshed.waveform == Waveform::Written
                      && perf(fresh.mdb, kUnanalysed, "overviewWaveFormData") != seabassBefore,
                  "1b. a waveform Seabass completed the row with, the grid the ledger's, is refreshed");
        }
        // A row whose flag is down over the player's analysis (one it broke
        // off; seen on a real stick): its waveform stays too, so it is not
        // completed, and its grid waits for the player with it. Told to, it
        // is completed as before.
        for (const bool over : {false, true}) {
            const Copy broken = freshCopy(over ? "players-waveform-flag-down-over" : "players-waveform-flag-down");
            exec(broken.mdb, "UPDATE Track SET isAnalyzed = 0 WHERE id = " + std::to_string(kAnalysed));
            check(!perf(broken.mdb, kAnalysed, "overviewWaveFormData").empty()
                      && !perf(broken.mdb, kAnalysed, "beatData").empty(),
                  "1b. the flag-down row keeps the player's overview and beat data");
            const std::string dbBefore = fileBytes(pathFromUtf8(broken.mdb));
            const auto brokenFilesBefore = overviewFiles(broken);
            AnalysisImportOptions options;
            options.fileSampleRateHz = record.sample_rate;
            options.overwritePlayerWaveforms = over;
            auto w = unshiftedWriter(pathToUtf8(broken.root));
            const auto r = w.importAnalysis(std::to_string(kAnalysed), analysis, options);
            if (!over) {
                check(r.waveform == Waveform::PlayersWaveform && r.grid.outcome == Outcome::RowNotAnalysed
                          && !r.completedRow && r.overviewFileRelativePath.empty(),
                      "1b. flag down, the player's waveform there: left alone, not completed, grid "
                          + std::to_string(int(r.grid.outcome)) + " waveform " + std::to_string(int(r.waveform)));
                check(fileBytes(pathFromUtf8(broken.mdb)) == dbBefore && overviewFiles(broken) == brokenFilesBefore,
                      "1b. m.db and every overview file byte for byte as they were");

                // A grid fill (--unanalysed fill) leaves the ledger's grid as
                // both grids of the flag-down row: its overview is still the
                // player's, the ledger's word notwithstanding.
                application::BeatGridWriteOptions fill;
                fill.unanalysed = application::UnanalysedRowGrid::Fill;
                fill.fileSampleRateHz = record.sample_rate;
                fill.fileDurationMs = seconds * 1000.0;
                const auto filled = w.writeBeatGrid(std::to_string(kAnalysed), analysis.beats, fill);
                check(filled.outcome == Outcome::Written && !filled.gridFingerprint.empty(),
                      "1b. the flag-down row's grid filled, outcome " + std::to_string(int(filled.outcome)));
                const auto overviewFilled = perf(broken.mdb, kAnalysed, "overviewWaveFormData");
                const auto filesFilled = overviewFiles(broken);
                check(column(broken.mdb, "Track", "id", kAnalysed, "isAnalyzed") == "0", "1b. the fill leaves the flag down");
                AnalysisImportOptions afterFill = options;
                afterFill.ownGridFingerprint = filled.gridFingerprint;  // as the ledger says
                const auto r2 = w.importAnalysis(std::to_string(kAnalysed), analysis, afterFill);
                check(r2.waveform == Waveform::PlayersWaveform && !r2.completedRow,
                      "1b. after a fill, with the ledger's grid: the player's waveform left alone, waveform "
                          + std::to_string(int(r2.waveform)));
                check(perf(broken.mdb, kAnalysed, "overviewWaveFormData") == overviewFilled
                          && overviewFiles(broken) == filesFilled,
                      "1b. its overview byte for byte as the player left it");
            } else {
                check(r.completedRow && r.waveform == Waveform::Written && r.grid.outcome == Outcome::Written,
                      "1b. flag down, told to: completed, the player's waveform replaced");
            }
        }
        std::cout << "case 1b (the player's waveform) done\n";
    }

    // 2. The colour preview when there is no three-band one, over the row's
    // own length when the scrolling waveform's count is not there.
    {
        const Copy copy = freshCopy("colour");
        const TrackData record = TrackData::from_blob(perf(copy.mdb, kAnalysedToo, "trackData"));
        const double seconds = static_cast<double>(record.samples) / record.sample_rate;
        RekordboxAnalysis analysis;
        analysis.colourPreview = synthetic(1200, 6, 3);
        AnalysisImportOptions waveformOnly;
        waveformOnly.grids = false;
        waveformOnly.overwritePlayerWaveforms = true;
        auto writer = unshiftedWriter(pathToUtf8(copy.root));
        const std::string beatDataBefore = column(copy.mdb, "PerformanceData", "trackId", kAnalysedToo, "beatData");
        const auto result = writer.importAnalysis(std::to_string(kAnalysedToo), analysis, waveformOnly);
        check(result.grid.outcome == Outcome::NotRequested && result.waveform == Waveform::Written,
              "a waveform alone: the grid not requested, the waveform written");
        const auto overview = domain::overviewFromColourPreview(*analysis.colourPreview, seconds, seconds);
        check(Overview::from_blob(perf(copy.mdb, kAnalysedToo, "overviewWaveFormData"))
                  == expectedBlob(overview, static_cast<double>(record.samples)),
              "from the colour preview, over the row's length");
        check(column(copy.mdb, "PerformanceData", "trackId", kAnalysedToo, "beatData") == beatDataBefore,
              "and the grid left alone");

        RekordboxAnalysis none;
        none.beats = constantGrid(37.0, 500.0, seconds * 1000.0);
        const auto noPreview = writer.importAnalysis(std::to_string(kAnalysed), none);
        check(noPreview.waveform == Waveform::NoSourceWaveform && noPreview.grid.outcome == Outcome::Written,
              "an analysed row with no preview: no waveform, the grid still written");
        std::cout << "case 2 (colour preview) done\n";
    }

    // 3. A row the player has not analysed, completed; then handed back.
    {
        const Copy copy = freshCopy("completed");
        const auto perfBefore = performanceRows(copy.mdb);
        const auto tracksBefore = trackRows(copy.mdb);
        const auto filesBefore = overviewFiles(copy);
        check(column(copy.mdb, "Track", "id", kUnanalysed, "isAnalyzed") == "0"
                  && perfBefore.at(kUnanalysed).at(0) == "NULL",
              "the row has no analysis");

        RekordboxAnalysis analysis;
        analysis.beats = constantGrid(120.0, 480.0, 200000.0);
        analysis.threeBandPreview = synthetic(1200, 3, 4);
        analysis.scrollEntries = 30000;  // 200 s
        analysis.key = 7;
        analysis.bpm = 125.0;
        AnalysisImportOptions options;
        options.fileSampleRateHz = 48000.0;
        auto writer = unshiftedWriter(pathToUtf8(copy.root));
        const auto result = writer.importAnalysis(std::to_string(kUnanalysed), analysis, options);
        check(result.grid.outcome == Outcome::Written && result.waveform == Waveform::Written && result.completedRow,
              "the row is completed");
        check(result.overviewFileRelativePath == fileOf(kUnanalysed), "with its overview file");

        const std::uint64_t samples = domain::engineSampleCount(30000, 48000.0, false);
        const auto overview = domain::overviewFromThreeBandPreview(*analysis.threeBandPreview, 200.0, samples / 48000.0);
        const auto rawRecord = inflated(perf(copy.mdb, kUnanalysed, "trackData"));
        check(rawRecord.size() == 68, "the analysis record is 68 raw bytes, " + std::to_string(rawRecord.size()));
        const TrackData record = TrackData::from_blob(perf(copy.mdb, kUnanalysed, "trackData"));
        const auto loudness = domain::loudnessFromOverview(overview);
        check(record.sample_rate == 48000.0 && record.samples == static_cast<std::int64_t>(samples) && record.key == 7,
              "rate, length and key in the record");
        check(record.average_loudness_low == loudness[0] && record.average_loudness_mid == loudness[1]
                  && record.average_loudness_high == loudness[2],
              "the loudness estimated from the overview");
        check(record.extra_data == std::vector<std::byte>(24, std::byte{0}), "24 zero bytes after it");
        check(Overview::from_blob(perf(copy.mdb, kUnanalysed, "overviewWaveFormData"))
                  == expectedBlob(overview, static_cast<double>(samples)),
              "the overview");
        const BeatData beatData = BeatData::from_blob(perf(copy.mdb, kUnanalysed, "beatData"));
        check(beatData.sample_rate == 48000.0 && beatData.samples == static_cast<double>(samples)
                  && beatData.is_beatgrid_set == 1 && beatData.default_beat_grid == beatData.adjusted_beat_grid
                  && beatData.adjusted_beat_grid.size() >= 2
                  && beatData.extra_data == std::vector<std::byte>(9, std::byte{0}),
              "both grids rekordbox's, at the file's rate and the counted length, 9 zero bytes after");
        check(Overview::from_blob(asBytes(fileBytes(copy.root / fileOf(kUnanalysed))))
                  == expectedBlob(domain::secondWaveformFromOverview(overview), static_cast<double>(samples)),
              "the overview file");
        check(column(copy.mdb, "Track", "id", kUnanalysed, "isAnalyzed") == "1", "the row is flagged analysed");
        check(column(copy.mdb, "Track", "id", kUnanalysed, "bpmAnalyzed") == "125.0"
                  && column(copy.mdb, "Track", "id", kUnanalysed, "bpm") == "125"
                  && column(copy.mdb, "Track", "id", kUnanalysed, "key") == "7",
              "with rekordbox's tempo and key");

        // Through the project's readers and libdjinterop's own track.
        check(readsBack(pathToUtf8(copy.root), kUnanalysed, analysis.beats, samples / 48.0),
              "the grid reads back through the project's reader");
        const auto read = readTrackAnalysis(pathToUtf8(copy.root), std::to_string(kUnanalysed));
        check(!read.waveform.empty() && !read.beats.empty(), "the waveform reader finds a waveform and a grid");
        {
            auto db = djinterop::engine::load_database(pathToUtf8(copy.root));
            auto track = db.track_by_id(kUnanalysed);
            check(track && track->sample_rate() == 48000.0 && track->sample_count() == samples,
                  "libdjinterop's track reads the rate and length");
            check(track && track->key() == djinterop::musical_key{7} && track->bpm() == 125.0,
                  "and the key and tempo");
            check(track && track->waveform().size() > 0, "and a waveform");
        }

        auto perfOthersBefore = perfBefore;
        auto perfOthersAfter = performanceRows(copy.mdb);
        auto tracksOthersBefore = tracksBefore;
        auto tracksOthersAfter = trackRows(copy.mdb);
        for (auto *rows : {&perfOthersBefore, &perfOthersAfter, &tracksOthersBefore, &tracksOthersAfter}) {
            rows->erase(kUnanalysed);
        }
        check(perfOthersAfter == perfOthersBefore && tracksOthersAfter == tracksOthersBefore, "no other row changed");
        auto filesAfter = overviewFiles(copy);
        filesAfter.erase(fileOf(kUnanalysed));
        check(filesAfter == filesBefore, "no other overview file changed");

        // Handed back to the player: the analysis columns as they were.
        std::string error;
        check(markForDeviceAnalysis(copy.mdb, {kUnanalysed}, &error) == 1, "markForDeviceAnalysis: " + error);
        const auto handedBack = performanceRows(copy.mdb).at(kUnanalysed);
        check(handedBack.at(0) == perfBefore.at(kUnanalysed).at(0) && handedBack.at(1) == perfBefore.at(kUnanalysed).at(1)
                  && handedBack.at(2) == perfBefore.at(kUnanalysed).at(2),
              "trackData, overview and beat data are what a never-analysed row has");
        check(column(copy.mdb, "Track", "id", kUnanalysed, "isAnalyzed") == "0", "and the flag is down");
        std::cout << "case 3 (completed row) done\n";
    }

    // 4. Each missing precondition: nothing written anywhere, and why.
    {
        const Copy copy = freshCopy("preconditions");
        exec(copy.mdb, "UPDATE Track SET isBeatGridLocked = 1 WHERE id = " + std::to_string(kUnanalysedToo));
        RekordboxAnalysis full;
        full.beats = constantGrid(120.0, 480.0, 200000.0);
        full.threeBandPreview = synthetic(1200, 3, 5);
        full.scrollEntries = 30000;
        AnalysisImportOptions withRate;
        withRate.fileSampleRateHz = 44100.0;

        struct Case
        {
            std::string what;
            std::int64_t id;
            RekordboxAnalysis analysis;
            AnalysisImportOptions options;
            Outcome grid;
            Waveform waveform;
        };
        std::vector<Case> cases;
        AnalysisImportOptions noGrids = withRate;
        noGrids.grids = false;
        cases.push_back({"grids not asked for", kUnanalysed, full, noGrids, Outcome::NotRequested, Waveform::RowNotAnalysed});
        AnalysisImportOptions noWaveforms = withRate;
        noWaveforms.waveforms = false;
        cases.push_back({"waveforms not asked for", kUnanalysed, full, noWaveforms, Outcome::RowNotAnalysed,
                         Waveform::NotRequested});
        RekordboxAnalysis oneBeat = full;
        oneBeat.beats.resize(1);
        cases.push_back({"one beat", kUnanalysed, oneBeat, withRate, Outcome::NoSourceGrid, Waveform::RowNotAnalysed});
        RekordboxAnalysis noPreview = full;
        noPreview.threeBandPreview.reset();
        cases.push_back({"no preview", kUnanalysed, noPreview, withRate, Outcome::RowNotAnalysed, Waveform::NoSourceWaveform});
        RekordboxAnalysis badPreview = full;
        badPreview.threeBandPreview = std::string(1201, '\x10');
        cases.push_back({"a preview that is not whole entries", kUnanalysed, badPreview, withRate, Outcome::RowNotAnalysed,
                         Waveform::NoSourceWaveform});
        RekordboxAnalysis noScroll = full;
        noScroll.scrollEntries.reset();
        cases.push_back({"no scrolling waveform count", kUnanalysed, noScroll, withRate, Outcome::RowNotAnalysed,
                         Waveform::NoSampleCount});
        cases.push_back({"no sample rate", kUnanalysed, full, {}, Outcome::RowNotAnalysed, Waveform::NoSampleCount});
        cases.push_back({"a locked row", kUnanalysedToo, full, withRate, Outcome::RowLocked, Waveform::RowNotAnalysed});

        const std::string dbBefore = fileBytes(pathFromUtf8(copy.mdb));
        const auto filesBefore = overviewFiles(copy);
        int hookCalls = 0;
        auto writer = unshiftedWriter(pathToUtf8(copy.root));
        writer.setBeforeBeatGridWrite([&] { ++hookCalls; });
        for (const Case &c : cases) {
            const auto result = writer.importAnalysis(std::to_string(c.id), c.analysis, c.options);
            check(result.grid.outcome == c.grid, c.what + ": grid outcome " + std::to_string(int(result.grid.outcome)));
            check(result.waveform == c.waveform, c.what + ": waveform outcome " + std::to_string(int(result.waveform)));
            check(!result.completedRow && result.overviewFileRelativePath.empty(), c.what + ": nothing completed");
        }
        check(hookCalls == 0, "no hook called, " + std::to_string(hookCalls));
        check(fileBytes(pathFromUtf8(copy.mdb)) == dbBefore, "m.db is byte for byte as it was");
        check(overviewFiles(copy) == filesBefore, "and every overview file");

        // The lock is the user's word, unless told to write over it.
        AnalysisImportOptions overwrite = withRate;
        overwrite.overwriteEngineEdits = true;
        const auto forced = writer.importAnalysis(std::to_string(kUnanalysedToo), full, overwrite);
        check(forced.completedRow && forced.grid.outcome == Outcome::Written, "a locked row completed when told to");
        std::cout << "case 4 (preconditions) done\n";
    }

    // 5. A preview: the real call's outcomes, nothing written.
    {
        const Copy copy = freshCopy("preview");
        const TrackData record = TrackData::from_blob(perf(copy.mdb, kAnalysed, "trackData"));
        const double seconds = static_cast<double>(record.samples) / record.sample_rate;
        RekordboxAnalysis analysed;
        analysed.beats = constantGrid(37.0, 60000.0 / 121.0, seconds * 1000.0);
        analysed.threeBandPreview = synthetic(1200, 3, 6);
        analysed.scrollEntries = static_cast<std::uint32_t>(seconds * 150.0);
        RekordboxAnalysis unanalysed = analysed;
        unanalysed.scrollEntries = 30000;
        AnalysisImportOptions withRate;
        withRate.fileSampleRateHz = 44100.0;
        withRate.fileIsMp3 = true;
        withRate.overwritePlayerWaveforms = true;
        const std::vector<std::pair<std::int64_t, RekordboxAnalysis>> calls = {
            {kAnalysed, analysed}, {kUnanalysed, unanalysed}, {kUnanalysedToo, RekordboxAnalysis{}}};

        const std::string dbBefore = fileBytes(pathFromUtf8(copy.mdb));
        const auto filesBefore = overviewFiles(copy);
        int hookCalls = 0;
        std::vector<AnalysisImportResult> previews;
        {
            auto preview = unshiftedWriter(pathToUtf8(copy.root));
            preview.setBeforeBeatGridWrite([&] { ++hookCalls; });
            preview.setOverviewFileSink([&](const std::string &, const std::string &) { ++hookCalls; });
            for (const auto &[id, analysis] : calls) {
                AnalysisImportOptions options = withRate;
                options.previewOnly = true;
                previews.push_back(preview.importAnalysis(std::to_string(id), analysis, options));
            }
        }
        check(hookCalls == 0, "a preview calls neither hook nor sink");
        check(fileBytes(pathFromUtf8(copy.mdb)) == dbBefore, "a preview leaves m.db as it was");
        check(overviewFiles(copy) == filesBefore, "and every overview file");
        auto real = unshiftedWriter(pathToUtf8(copy.root));
        for (std::size_t i = 0; i < calls.size(); ++i) {
            const auto done = real.importAnalysis(std::to_string(calls[i].first), calls[i].second, withRate);
            const std::string who = "call " + std::to_string(i);
            check(previews[i].grid.outcome == done.grid.outcome && previews[i].waveform == done.waveform,
                  who + ": the preview's outcomes are the real call's");
            check(previews[i].grid.gridFingerprint == done.grid.gridFingerprint
                      && previews[i].completedRow == done.completedRow
                      && previews[i].overviewFileRelativePath == done.overviewFileRelativePath,
                  who + ": and its fingerprint, completion and file");
        }
        check(previews[0].waveform == Waveform::Written && previews[1].completedRow
                  && previews[2].grid.outcome == Outcome::NoSourceGrid,
              "the calls are what they are meant to be");
        std::cout << "case 5 (preview) done\n";
    }

    // 6. Leaving the file out, writing zero loudness.
    {
        const Copy copy = freshCopy("hardware-options");
        RekordboxAnalysis analysis;
        analysis.beats = constantGrid(120.0, 480.0, 200000.0);
        analysis.threeBandPreview = synthetic(1200, 3, 7);
        analysis.scrollEntries = 30000;
        AnalysisImportOptions options;
        options.fileSampleRateHz = 44100.0;
        options.skipOverviewFile = true;
        options.zeroLoudness = true;
        const auto filesBefore = overviewFiles(copy);
        const std::string bpmBefore = column(copy.mdb, "Track", "id", kUnanalysed, "bpmAnalyzed");
        const std::string keyBefore = column(copy.mdb, "Track", "id", kUnanalysed, "key");
        auto writer = unshiftedWriter(pathToUtf8(copy.root));
        const auto result = writer.importAnalysis(std::to_string(kUnanalysed), analysis, options);
        check(result.completedRow && result.overviewFileRelativePath.empty(), "completed, no file named");
        check(overviewFiles(copy) == filesBefore, "and no file written");
        const TrackData record = TrackData::from_blob(perf(copy.mdb, kUnanalysed, "trackData"));
        check(record.average_loudness_low == 0.0 && record.average_loudness_mid == 0.0
                  && record.average_loudness_high == 0.0,
              "zero loudness");
        check(record.key == 0, "and key 0 when rekordbox has none");
        check(column(copy.mdb, "Track", "id", kUnanalysed, "bpmAnalyzed") == bpmBefore
                  && column(copy.mdb, "Track", "id", kUnanalysed, "key") == keyBefore,
              "no tempo or key given leaves the row's");
        std::cout << "case 6 (hardware options) done\n";
    }

    // 7. The sink and the hook, in order; and a sink that fails.
    {
        const Copy copy = freshCopy("sink");
        RekordboxAnalysis analysis;
        analysis.beats = constantGrid(120.0, 480.0, 200000.0);
        analysis.threeBandPreview = synthetic(1200, 3, 8);
        analysis.scrollEntries = 30000;
        AnalysisImportOptions options;
        options.fileSampleRateHz = 44100.0;
        const std::string dbBefore = fileBytes(pathFromUtf8(copy.mdb));
        const auto filesBefore = overviewFiles(copy);
        std::vector<std::string> events;
        std::string sunkPath;
        std::string sunkBytes;
        auto writer = unshiftedWriter(pathToUtf8(copy.root));
        writer.setBeforeBeatGridWrite([&] {
            events.push_back("hook");
            check(fileBytes(pathFromUtf8(copy.mdb)) == dbBefore, "at the hook m.db is as it was");
        });
        writer.setOverviewFileSink([&](const std::string &relative, const std::string &bytes) {
            events.push_back("sink");
            sunkPath = relative;
            sunkBytes = bytes;
            check(column(copy.mdb, "Track", "id", kUnanalysed, "isAnalyzed") == "0",
                  "at the sink the flag is still down");
            // The file first, then every column in one transaction.
            check(column(copy.mdb, "PerformanceData", "trackId", kUnanalysed, "trackData") == "NULL",
                  "and the record not written yet");
        });
        const auto result = writer.importAnalysis(std::to_string(kUnanalysed), analysis, options);
        check((events == std::vector<std::string>{"hook", "sink"}), "the hook, then the sink, once each");
        check(sunkPath == fileOf(kUnanalysed) && sunkPath == result.overviewFileRelativePath, "the sink gets the path");
        const std::uint64_t samples = domain::engineSampleCount(30000, 44100.0, false);
        const auto overview = domain::overviewFromThreeBandPreview(*analysis.threeBandPreview, 200.0, samples / 44100.0);
        check(Overview::from_blob(asBytes(sunkBytes))
                  == expectedBlob(domain::secondWaveformFromOverview(overview), static_cast<double>(samples)),
              "and the bytes");
        check(overviewFiles(copy) == filesBefore, "with a sink, the writer writes no file itself");
        check(column(copy.mdb, "Track", "id", kUnanalysed, "isAnalyzed") == "1", "the flag is up afterwards");

        // A sink that throws: the call throws, and the row stays one the
        // player is still to analyse.
        auto failing = unshiftedWriter(pathToUtf8(copy.root));
        failing.setOverviewFileSink([](const std::string &, const std::string &) {
            throw std::runtime_error("the stick is gone");
        });
        bool threw = false;
        try {
            failing.importAnalysis(std::to_string(kUnanalysedToo), analysis, options);
        } catch (const std::exception &) {
            threw = true;
        }
        check(threw, "a sink that throws fails the call");
        check(column(copy.mdb, "Track", "id", kUnanalysedToo, "isAnalyzed") == "0"
                  && column(copy.mdb, "PerformanceData", "trackId", kUnanalysedToo, "trackData") == "NULL",
              "and leaves the row as it was, not half analysed");
        std::cout << "case 7 (sink and hook) done\n";
    }

    // 8. Engine 2.18 keeps it all in Track; 1.x is not written.
    {
        const fs::path dir = testing::scratchRoot() / "engine_analysis_import" / "schema-2";
        fs::remove_all(dir);
        fs::create_directories(dir);
        const std::string root = pathToUtf8(dir / "Engine Library");
        std::int64_t analysedId = 0;
        std::int64_t unanalysedId = 0;
        {
            auto db = djinterop::engine::create_database(root, djinterop::engine::engine_schema::schema_2_18_0);
            djinterop::track_snapshot snapshot;
            snapshot.title = "Subject";
            snapshot.relative_path = "subject.wav";
            snapshot.sample_rate = 44100.0;
            snapshot.sample_count = 44100ull * 120;
            snapshot.duration = std::chrono::milliseconds(120000);
            snapshot.bpm = 120.0;
            snapshot.beatgrid = {{-4, -2.0 * 22050}, {236, 238.0 * 22050}};
            snapshot.waveform = std::vector<djinterop::waveform_entry>(
                1024, djinterop::waveform_entry{{10, 255}, {20, 255}, {30, 255}});
            analysedId = db.create_track(snapshot).id();
            snapshot.relative_path = "other.wav";
            unanalysedId = db.create_track(snapshot).id();
        }
        const std::string mdb = pathToUtf8(dir / "Engine Library" / "Database2" / "m.db");
        std::string error;
        check(markForDeviceAnalysis(mdb, {unanalysedId}, &error) == 1, "2.18: a row made unanalysed: " + error);
        // Both rows as Engine's rekordbox import leaves them (case 10).
        exec(mdb, "UPDATE Track SET thirdPartySourceId = 1");

        RekordboxAnalysis analysis;
        analysis.beats = constantGrid(250.0, 480.0, 120000.0);
        analysis.threeBandPreview = synthetic(1200, 3, 9);
        analysis.scrollEntries = 18000;
        analysis.bpm = 125.0;
        // Not told to write over it, on a copy: the analysed row keeps the
        // overview in its Track row, the unanalysed row is completed.
        {
            const fs::path offDir = testing::scratchRoot() / "engine_analysis_import" / "schema-2-off";
            fs::remove_all(offDir);
            fs::copy(dir, offDir, fs::copy_options::recursive);
            const std::string offMdb = pathToUtf8(offDir / "Engine Library" / "Database2" / "m.db");
            const auto overviewBefore = blobOf(offMdb, "Track", "id", analysedId, "overviewWaveFormData");
            check(!overviewBefore.empty(), "2.18: the analysed row has an overview in Track");
            AnalysisImportOptions off;
            off.fileSampleRateHz = 44100.0;
            auto offWriter = unshiftedWriter(pathToUtf8(offDir / "Engine Library"));
            const auto kept = offWriter.importAnalysis(std::to_string(analysedId), analysis, off);
            check(kept.grid.outcome == Outcome::Written && kept.waveform == Waveform::PlayersWaveform,
                  "2.18, option off: the analysed row's grid written, its waveform left alone");
            check(blobOf(offMdb, "Track", "id", analysedId, "overviewWaveFormData") == overviewBefore,
                  "2.18, option off: Track's overview byte for byte as it was");
            const auto completedOff = offWriter.importAnalysis(std::to_string(unanalysedId), analysis, off);
            check(completedOff.completedRow && completedOff.waveform == Waveform::Written,
                  "2.18, option off: the unanalysed row still completed");
        }

        AnalysisImportOptions options;
        options.fileSampleRateHz = 44100.0;
        options.overwritePlayerWaveforms = true;
        auto writer = unshiftedWriter(root);
        const auto analysedResult = writer.importAnalysis(std::to_string(analysedId), analysis, options);
        check(analysedResult.grid.outcome == Outcome::Written && analysedResult.waveform == Waveform::Written,
              "2.18: an analysed row's grid and waveform written");
        auto library = djinterop::engine::v2::engine_library::load(root);
        const auto overview = domain::overviewFromThreeBandPreview(*analysis.threeBandPreview, 120.0, 120.0);
        check(library.track().get_overview_waveform_data(analysedId) == expectedBlob(overview, 44100.0 * 120),
              "2.18: the overview in the Track table");
        const auto completed = writer.importAnalysis(std::to_string(unanalysedId), analysis, options);
        check(completed.completedRow, "2.18: an unanalysed row completed");
        check(column(mdb, "Track", "id", analysedId, "thirdPartySourceId") == "NULL"
                  && column(mdb, "Track", "id", unanalysedId, "thirdPartySourceId") == "NULL",
              "2.18: both rows lose the mark of Engine's rekordbox import, in the Track table");
        check(library.track().get_is_analyzed(unanalysedId), "2.18: flagged analysed");
        const auto raw = inflated(blobOf(mdb, "Track", "id", unanalysedId, "trackData"));
        check(raw.size() == 44, "2.18: the record ends after the loudness, " + std::to_string(raw.size()) + " bytes");
        check(library.track().get_bpm_analyzed(unanalysedId) == 125.0 && library.track().get_bpm(unanalysedId) == 125,
              "2.18: the tempo");
        const std::string uuid = library.information().get().uuid;
        check(fs::exists(dir / "Engine Library" / "Database2" / "OverviewData" / uuid
                         / (std::to_string(unanalysedId) + ".rgb")),
              "2.18: the overview file in that database's uuid folder");

        const fs::path dir1 = testing::scratchRoot() / "engine_analysis_import" / "schema-1";
        fs::remove_all(dir1);
        fs::create_directories(dir1);
        const std::string root1 = pathToUtf8(dir1 / "Engine Library");
        std::int64_t id1 = 0;
        {
            auto db = djinterop::engine::create_database(root1, djinterop::engine::engine_schema::schema_1_18_0_os);
            djinterop::track_snapshot snapshot;
            snapshot.title = "Subject";
            snapshot.relative_path = "subject.wav";
            id1 = db.create_track(snapshot).id();
        }
        LibdjinteropEngineCueWriter writer1(root1);
        const auto old = writer1.importAnalysis(std::to_string(id1), analysis, options);
        check(old.grid.outcome == Outcome::UnsupportedSchema && old.waveform == Waveform::UnsupportedSchema,
              "Engine 1.x is not written");
        std::cout << "case 8 (schema 2.18 and 1.x) done\n";
    }

    // 9. A Prime 4's own overview file decodes with the code that encodes
    // ours: the same container as the overview column, 1024 points over
    // the row's length. Re-encoded it decodes to the same content (the
    // compressed bytes may differ with zlib's level).
    {
        const Copy copy = freshCopy("container");
        const std::int64_t id = 10;
        const std::string real = fileBytes(copy.root / fileOf(id));
        check(real.size() > 4 && static_cast<unsigned char>(real[0]) == 0 && static_cast<unsigned char>(real[1]) == 0
                  && static_cast<unsigned char>(real[2]) == 0x0c && static_cast<unsigned char>(real[3]) == 0x1b,
              "Denon's file starts with the raw length 3099, big-endian");
        const Overview decoded = Overview::from_blob(asBytes(real));
        const TrackData record = TrackData::from_blob(perf(copy.mdb, id, "trackData"));
        check(decoded.waveform_points.size() == 1024 && decoded.extra_data.empty(), "1024 points and nothing after");
        check(decoded.samples_per_waveform_point == static_cast<double>(record.samples) / 1024.0,
              "each point the row's samples / 1024");
        const auto reencoded = decoded.to_blob();
        check(Overview::from_blob(reencoded) == decoded, "re-encoded, it decodes to the same");
        check(inflated(reencoded) == inflated(asBytes(real)), "and inflates to the same 3099 bytes");

        RekordboxAnalysis analysis;
        analysis.threeBandPreview = synthetic(1200, 3, 10);
        AnalysisImportOptions waveformOnly;
        waveformOnly.grids = false;
        waveformOnly.overwritePlayerWaveforms = true;
        auto writer = unshiftedWriter(pathToUtf8(copy.root));
        check(writer.importAnalysis(std::to_string(id), analysis, waveformOnly).waveform == Waveform::Written,
              "ours written over it");
        const std::string ours = fileBytes(copy.root / fileOf(id));
        check(ours.substr(0, 4) == real.substr(0, 4) && inflated(asBytes(ours)).size() == 3099,
              "ours has the same header and raw length");
        std::cout << "case 9 (overview file container) done\n";
    }

    // 10. The mark of Engine's rekordbox import (thirdPartySourceId = 1). A
    // player that loads a marked row for the first time takes its grid and
    // cues for rekordbox's timeline and moves them about 52 ms earlier (the
    // Prime 4 and the Prime Go+, 2026-11-22). A grid written here is on
    // Engine's timeline already, so the mark goes with it. A marked row's
    // cues are moved onto Engine's timeline in the same write, by the shift
    // the cue writer takes off, when that shift is known. Here it is not
    // (unshiftedWriter(): no MP3's lead-in reads), so a marked MP3 row
    // with cues keeps cues and mark, for the player to move; a file that
    // is not an MP3 has a known shift of 0, and loses the mark with its
    // cues byte for byte. A row whose cues cannot be read keeps both.
    {
        const Copy copy = freshCopy("rekordbox-import-mark");
        constexpr std::int64_t kUnanalysedWithHotCue = 17;
        constexpr std::int64_t kAnalysedWithHotCue = 69;
        const auto mark = [&](std::int64_t id) {
            return column(copy.mdb, "PerformanceData", "trackId", id, "thirdPartySourceId");
        };
        for (const std::int64_t id :
             {kUnanalysed, kUnanalysedToo, kAnalysed, kAnalysedToo, kUnanalysedWithHotCue, kAnalysedWithHotCue}) {
            check(mark(id) == "1", "the fixture's row " + std::to_string(id) + " carries the mark");
        }
        // Rows with a loop alone, a main cue alone, and cues that cannot be
        // read, each given the mark.
        constexpr std::int64_t kWithLoopOnly = 30;
        constexpr std::int64_t kWithMainCueOnly = 377;
        constexpr std::int64_t kWithDamagedCues = 3;
        // A stream that does not end within the blob: libdjinterop's own
        // inflate never comes back from this one.
        constexpr std::int64_t kWithCutShortCues = 4;
        exec(copy.mdb, "UPDATE PerformanceData SET thirdPartySourceId = 1 WHERE trackId IN (30, 377);"
                       "UPDATE PerformanceData SET quickCues = x'0102' WHERE trackId = 3;"
                       "UPDATE PerformanceData SET quickCues = x'00000064789c63' WHERE trackId = 4;");
        {
            auto db = djinterop::engine::load_database(pathToUtf8(copy.root));
            const auto cuesOf = [&](std::int64_t id) {
                const auto track = *db.track_by_id(id);
                const auto hotCues = track.hot_cues();
                const auto loops = track.loops();
                const auto set = [](const auto &slot) { return slot.has_value(); };
                const auto mainCue = track.main_cue();
                return std::string(std::any_of(hotCues.begin(), hotCues.end(), set) ? "H" : "-")
                    + (std::any_of(loops.begin(), loops.end(), set) ? "L" : "-") + (mainCue && *mainCue > 0.0 ? "M" : "-");
            };
            check(cuesOf(kUnanalysed) == "---" && cuesOf(kAnalysed) == "---" && cuesOf(kAnalysedToo) == "---",
                  "the rows that lose the mark carry no cue");
            check(cuesOf(kUnanalysedWithHotCue).front() == 'H' && cuesOf(kAnalysedWithHotCue).front() == 'H',
                  "the hot-cue rows carry one");
            check(cuesOf(kWithLoopOnly) == "-L-", "row 30 carries a loop alone: " + cuesOf(kWithLoopOnly));
            check(cuesOf(kWithMainCueOnly) == "--M", "row 377 a main cue alone: " + cuesOf(kWithMainCueOnly));
        }
        const auto others = [&] {
            auto marks = rows(copy.mdb, "SELECT trackId, quote(thirdPartySourceId) FROM PerformanceData");
            for (const std::int64_t id : {kUnanalysed, kAnalysed, kAnalysedToo, std::int64_t{618}}) {
                marks.erase(id);
            }
            return marks;
        };
        const auto othersBefore = others();

        RekordboxAnalysis analysis;
        analysis.beats = constantGrid(120.0, 480.0, 200000.0);
        analysis.threeBandPreview = synthetic(1200, 3, 11);
        analysis.scrollEntries = 30000;
        analysis.bpm = 125.0;
        AnalysisImportOptions options;
        options.fileSampleRateHz = 44100.0;
        auto writer = unshiftedWriter(pathToUtf8(copy.root));

        const auto completed = writer.importAnalysis(std::to_string(kUnanalysed), analysis, options);
        check(completed.completedRow && completed.grid.outcome == Outcome::Written, "a row completed with a grid");
        check(mark(kUnanalysed) == "NULL", "loses the mark: " + mark(kUnanalysed));

        const auto replaced = writer.importAnalysis(std::to_string(kAnalysed), analysis, options);
        check(replaced.grid.outcome == Outcome::Written, "an analysed row given rekordbox's grid");
        check(mark(kAnalysed) == "NULL", "loses the mark: " + mark(kAnalysed));

        const auto gridOnly = writer.writeBeatGrid(std::to_string(kAnalysedToo), analysis.beats);
        check(gridOnly.outcome == Outcome::Written, "a grid written alone");
        check(mark(kAnalysedToo) == "NULL", "takes the mark off too: " + mark(kAnalysedToo));

        // An MP3 row with readable cues and no known shift: the grid is
        // written, cues and mark stay.
        for (const auto &[id, what] : std::vector<std::pair<std::int64_t, std::string>>{
                 {kUnanalysedWithHotCue, "an unanalysed row with a hot cue"},
                 {kAnalysedWithHotCue, "an analysed row with a hot cue"},
                 {kWithLoopOnly, "a row with a loop alone"},
                 {kWithMainCueOnly, "a row with a main cue alone"}}) {
            const auto beatDataBefore = perf(copy.mdb, id, "beatData");
            const auto quickCuesBefore = perf(copy.mdb, id, "quickCues");
            const auto loopsBefore = perf(copy.mdb, id, "loops");
            const auto cued = writer.importAnalysis(std::to_string(id), analysis, options);
            check(cued.grid.outcome == Outcome::Written && perf(copy.mdb, id, "beatData") != beatDataBefore,
                  what + " gets the grid");
            check(perf(copy.mdb, id, "quickCues") == quickCuesBefore && perf(copy.mdb, id, "loops") == loopsBefore,
                  what + " keeps its cues byte for byte");
            check(mark(id) == "1", what + ", its shift not known, keeps the mark: " + mark(id));
        }
        // A file that is not an MP3: its shift is 0, known. Row 618 (hot
        // cues and a loop, marked) named an .m4a in this copy.
        {
            constexpr std::int64_t kNotAnMp3 = 618;
            exec(copy.mdb, "UPDATE Track SET path = replace(path, '.mp3', '.m4a') WHERE id = 618;");
            check(mark(kNotAnMp3) == "1", "row 618 carries the mark");
            const auto quickCuesBefore = perf(copy.mdb, kNotAnMp3, "quickCues");
            const auto loopsBefore = perf(copy.mdb, kNotAnMp3, "loops");
            const auto cued = writer.importAnalysis(std::to_string(kNotAnMp3), analysis, options);
            check(cued.grid.outcome == Outcome::Written, "a marked M4A row with cues gets the grid");
            check(perf(copy.mdb, kNotAnMp3, "quickCues") == quickCuesBefore
                      && perf(copy.mdb, kNotAnMp3, "loops") == loopsBefore,
                  "and keeps its cues byte for byte");
            check(mark(kNotAnMp3) == "NULL", "and loses the mark: " + mark(kNotAnMp3));
        }
        // Cues that cannot be read: written around, cues and mark left.
        for (const auto &[id, what] : std::vector<std::pair<std::int64_t, std::string>>{
                 {kWithDamagedCues, "a row whose cues cannot be read"},
                 {kWithCutShortCues, "a row whose cues are cut short"}}) {
            const auto beatDataBefore = perf(copy.mdb, id, "beatData");
            const auto quickCuesBefore = perf(copy.mdb, id, "quickCues");
            const auto cued = writer.importAnalysis(std::to_string(id), analysis, options);
            check(cued.grid.outcome == Outcome::Written && perf(copy.mdb, id, "beatData") != beatDataBefore,
                  what + " gets the grid");
            check(perf(copy.mdb, id, "quickCues") == quickCuesBefore, what + " keeps its cues");
            check(mark(id) == "1", what + " keeps the mark: " + mark(id));
        }

        AnalysisImportOptions waveformOnly = options;
        waveformOnly.grids = false;
        const auto noGrid = writer.importAnalysis(std::to_string(kUnanalysedToo), analysis, waveformOnly);
        check(noGrid.grid.outcome == Outcome::NotRequested, "an import that asks for no grid");
        check(mark(kUnanalysedToo) == "1", "leaves the mark: " + mark(kUnanalysedToo));

        check(others() == othersBefore, "no other row's mark changed");
        check(writer.memoryCuesDroppedByLeadInShift() == 0 && writer.loopsDroppedByLeadInShift() == 0,
              "nothing left out without a shift");
        std::cout << "case 10 (the mark of Engine's rekordbox import) done\n";
    }

    // 11. The same with a shift: the writer is told the rows' MP3s have a
    // LAME lead-in, so a marked row's cues move 51.18 ms earlier as the
    // grid goes in, under writeHotCues()' rules (a hot cue clamped at 0, a
    // loop the shift empties left out, a loop's start clamped, a main cue
    // moved to 0 or before left out), labels and colours as they were. A
    // row without the mark keeps its cues byte for byte.
    {
        const Copy copy = freshCopy("rekordbox-import-mark-shifted");
        const auto leadIn =
            infrastructure::audio::readMp3LeadIn(pathToUtf8(g_fixtures / "mp3-lead-in" / "lame-cbr-44100.mp3"));
        check(leadIn.has_value(), "the LAME fixture's lead-in reads");
        LibdjinteropEngineCueWriter writer(pathToUtf8(copy.root));
        int asked = 0;
        writer.setLeadInSource([&](const std::string &) {
            ++asked;
            return leadIn;
        });
        const auto mark = [&](std::int64_t id) {
            return column(copy.mdb, "PerformanceData", "trackId", id, "thirdPartySourceId");
        };
        constexpr std::int64_t kUnanalysedWithHotCue = 17;
        constexpr std::int64_t kAnalysedWithHotCue = 69;
        constexpr std::int64_t kHotCueAndLoop = 618;
        constexpr std::int64_t kWithLoopOnly = 30;
        constexpr std::int64_t kWithMainCueOnly = 377;
        // Cues made for the edges: a hot cue and a loop inside the shift,
        // a loop that starts inside it and ends after, a main cue inside it.
        constexpr std::int64_t kAtTheStart = 45;
        constexpr std::int64_t kWithDamagedCues = 3;
        // A loop alone and no mark.
        constexpr std::int64_t kUnmarkedWithLoop = 36;
        exec(copy.mdb, "UPDATE PerformanceData SET thirdPartySourceId = 1 WHERE trackId IN (30, 377, 45);"
                       "UPDATE PerformanceData SET quickCues = x'0102' WHERE trackId = 3;");
        check(mark(kUnmarkedWithLoop) == "NULL" && mark(kHotCueAndLoop) == "1", "the fixture's marks for 36 and 618");
        {
            auto library = djinterop::engine::v3::engine_library::load(pathToUtf8(copy.root));
            auto performance = library.performance_data();
            QuickCuesBlob quickCues = performance.get_quick_cues(kAtTheStart);
            quickCues.quick_cues[0] = {"near", 1000.0, djinterop::pad_color{1, 2, 3, 255}};
            quickCues.adjusted_main_cue = 2000.0;
            quickCues.default_main_cue = 2000.0;
            performance.set_quick_cues(kAtTheStart, quickCues);
            LoopsBlob loops = performance.get_loops(kAtTheStart);
            loops.loops[0] = {"emptied", 500.0, 2000.0, true, true, djinterop::pad_color{4, 5, 6, 255}};
            loops.loops[1] = {"clamped", 1000.0, 50000.0, true, true, djinterop::pad_color{7, 8, 9, 255}};
            performance.set_loops(kAtTheStart, loops);
        }
        const double shiftMs = writer.cueTimelineShiftMs(std::to_string(kHotCueAndLoop));
        check(std::abs(shiftMs - 2257 * 1000.0 / 44100) < 1e-6, "the rows shift by LAME's 51.18 ms: " + std::to_string(shiftMs));

        RekordboxAnalysis analysis;
        analysis.beats = constantGrid(120.0, 480.0, 200000.0);
        analysis.threeBandPreview = synthetic(1200, 3, 12);
        analysis.scrollEntries = 30000;
        analysis.bpm = 125.0;
        AnalysisImportOptions options;
        options.fileSampleRateHz = 44100.0;

        // The row's rate as the reader counts its cues: the analysis
        // record's once the grid is in.
        const auto rateOf = [&](std::int64_t id) {
            return TrackData::from_blob(perf(copy.mdb, id, "trackData")).sample_rate;
        };
        const auto movedBy = [](double samples, double shift) { return std::max(0.0, samples - shift); };
        for (const auto &[id, what] : std::vector<std::pair<std::int64_t, std::string>>{
                 {kUnanalysedWithHotCue, "an unanalysed row with a hot cue"},
                 {kAnalysedWithHotCue, "an analysed row with a hot cue"},
                 {kHotCueAndLoop, "a row with a hot cue and a loop"},
                 {kWithLoopOnly, "a row with a loop alone"},
                 {kWithMainCueOnly, "a row with a main cue alone"}}) {
            const auto quickCuesBlobBefore = perf(copy.mdb, id, "quickCues");
            const QuickCuesBlob quickBefore = QuickCuesBlob::from_blob(quickCuesBlobBefore);
            const LoopsBlob loopsBefore = LoopsBlob::from_blob(perf(copy.mdb, id, "loops"));
            const int askedBefore = asked;
            const auto cued = writer.importAnalysis(std::to_string(id), analysis, options);
            check(cued.grid.outcome == Outcome::Written, what + " gets the grid");
            check(asked - askedBefore == 1, what + ": the file's start read once for grid and cues, not "
                                                + std::to_string(asked - askedBefore) + " times");
            check(mark(id) == "NULL", what + " loses the mark: " + mark(id));
            const double shift = shiftMs / 1000.0 * rateOf(id);
            const QuickCuesBlob quickAfter = QuickCuesBlob::from_blob(perf(copy.mdb, id, "quickCues"));
            const LoopsBlob loopsAfter = LoopsBlob::from_blob(perf(copy.mdb, id, "loops"));
            bool moved = quickAfter.quick_cues.size() == quickBefore.quick_cues.size()
                && loopsAfter.loops.size() == loopsBefore.loops.size();
            int cues = 0;
            for (std::size_t i = 0; moved && i < quickBefore.quick_cues.size(); ++i) {
                const auto &b = quickBefore.quick_cues[i];
                const auto &a = quickAfter.quick_cues[i];
                const double want = b.sample_offset < 0.0 ? b.sample_offset : movedBy(b.sample_offset, shift);
                cues += b.sample_offset >= 0.0 ? 1 : 0;
                moved = std::abs(a.sample_offset - want) < 1e-6 && a.label == b.label && a.color == b.color;
            }
            for (std::size_t i = 0; moved && i < loopsBefore.loops.size(); ++i) {
                const auto &b = loopsBefore.loops[i];
                const auto &a = loopsAfter.loops[i];
                if (!b.is_start_set && !b.is_end_set) {
                    moved = a == b;
                    continue;
                }
                ++cues;
                moved = std::abs(a.start_sample_offset - movedBy(b.start_sample_offset, shift)) < 1e-6
                    && std::abs(a.end_sample_offset - movedBy(b.end_sample_offset, shift)) < 1e-6 && a.label == b.label
                    && a.color == b.color && a.is_start_set && a.is_end_set;
            }
            if (quickBefore.adjusted_main_cue > 0.0) {
                ++cues;
                moved = moved && std::abs(quickAfter.adjusted_main_cue - (quickBefore.adjusted_main_cue - shift)) < 1e-6;
            } else {
                moved = moved && quickAfter.adjusted_main_cue == quickBefore.adjusted_main_cue;
            }
            check(moved && cues > 0, what + ": every cue " + std::to_string(shift) + " samples earlier, labels and colours kept");
            check(quickAfter.extra_data == quickBefore.extra_data && loopsAfter.extra_data == loopsBefore.extra_data,
                  what + ": what follows the cues kept");
        }

        // The edges, at 44.1 kHz: 2257 samples.
        {
            const auto cued = writer.importAnalysis(std::to_string(kAtTheStart), analysis, options);
            check(cued.grid.outcome == Outcome::Written && mark(kAtTheStart) == "NULL",
                  "the row with cues at the start gets the grid and loses the mark");
            const double shift = shiftMs / 1000.0 * rateOf(kAtTheStart);
            check(std::abs(shift - 2257.0) < 1e-6, "2257 samples at the row's rate");
            const QuickCuesBlob quickAfter = QuickCuesBlob::from_blob(perf(copy.mdb, kAtTheStart, "quickCues"));
            const LoopsBlob loopsAfter = LoopsBlob::from_blob(perf(copy.mdb, kAtTheStart, "loops"));
            check(quickAfter.quick_cues[0].sample_offset == 0.0 && quickAfter.quick_cues[0].label == "near",
                  "a hot cue inside the shift is clamped to 0");
            check(quickAfter.adjusted_main_cue == 0.0 && quickAfter.default_main_cue == 0.0,
                  "a main cue inside the shift is left out (0, no main cue)");
            check(!loopsAfter.loops[0].is_start_set && !loopsAfter.loops[0].is_end_set
                      && loopsAfter.loops[0].start_sample_offset == -1.0,
                  "a loop the shift empties is left out");
            check(loopsAfter.loops[1].start_sample_offset == 0.0 && loopsAfter.loops[1].end_sample_offset == 50000.0 - 2257.0
                      && loopsAfter.loops[1].label == "clamped",
                  "a loop that starts inside the shift: its start clamped, its end moved");
            check(writer.memoryCuesDroppedByLeadInShift() == 1 && writer.loopsDroppedByLeadInShift() == 1,
                  "the main cue and the loop left out are counted: "
                      + std::to_string(writer.memoryCuesDroppedByLeadInShift()) + ", "
                      + std::to_string(writer.loopsDroppedByLeadInShift()));
        }

        // Cues that cannot be read: left, with the mark.
        {
            const auto quickCuesBefore = perf(copy.mdb, kWithDamagedCues, "quickCues");
            const auto loopsBefore = perf(copy.mdb, kWithDamagedCues, "loops");
            const auto cued = writer.importAnalysis(std::to_string(kWithDamagedCues), analysis, options);
            check(cued.grid.outcome == Outcome::Written, "the row with damaged cues gets the grid");
            check(perf(copy.mdb, kWithDamagedCues, "quickCues") == quickCuesBefore
                      && perf(copy.mdb, kWithDamagedCues, "loops") == loopsBefore && mark(kWithDamagedCues) == "1",
                  "and keeps its cues and the mark: " + mark(kWithDamagedCues));
        }

        // No mark: the cues are not the grid copy's to move.
        {
            const auto quickCuesBefore = perf(copy.mdb, kUnmarkedWithLoop, "quickCues");
            const auto loopsBefore = perf(copy.mdb, kUnmarkedWithLoop, "loops");
            const auto beatDataBefore = perf(copy.mdb, kUnmarkedWithLoop, "beatData");
            const auto cued = writer.importAnalysis(std::to_string(kUnmarkedWithLoop), analysis, options);
            check(cued.grid.outcome == Outcome::Written && perf(copy.mdb, kUnmarkedWithLoop, "beatData") != beatDataBefore,
                  "an unmarked row with a loop gets the grid");
            check(perf(copy.mdb, kUnmarkedWithLoop, "quickCues") == quickCuesBefore
                      && perf(copy.mdb, kUnmarkedWithLoop, "loops") == loopsBefore
                      && mark(kUnmarkedWithLoop) == "NULL",
                  "and keeps its cues byte for byte, with no mark");
        }
        std::cout << "case 11 (a marked row's cues moved onto Engine's timeline with the grid) done\n";
    }

    // 11b. The same move against what a player did. A Prime 4 (Engine OS
    // 5.3.0) loaded a marked row once and stored every hot cue and both
    // ends of its loop 2292 samples earlier, the main cue (not set) left
    // at 0, labels and colours kept (tests/fixtures/prime4-first-load).
    // Seabass, told the same shift, leaves the row as the player did: a
    // loop moves with the hot cues, by the same amount.
    {
        const Copy copy = freshCopy("prime4-first-load");
        const fs::path capture = g_fixtures / "prime4-first-load";
        const auto bytesOf = [&](const char *name) {
            std::ifstream in(capture / name, std::ios::binary);
            const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::vector<std::byte> out(raw.size());
            std::transform(raw.begin(), raw.end(), out.begin(), [](char c) { return static_cast<std::byte>(c); });
            return out;
        };
        const auto hexOf = [](const std::vector<std::byte> &bytes) {
            static const char digits[] = "0123456789abcdef";
            std::string hex;
            for (const std::byte b : bytes) {
                hex += digits[std::to_integer<int>(b) >> 4];
                hex += digits[std::to_integer<int>(b) & 15];
            }
            return hex;
        };
        const auto markedQuick = bytesOf("marked-quickCues.bin");
        const auto markedLoops = bytesOf("marked-loops.bin");
        const auto loadedQuick = bytesOf("loaded-quickCues.bin");
        const auto loadedLoops = bytesOf("loaded-loops.bin");
        check(!markedQuick.empty() && !markedLoops.empty() && !loadedQuick.empty() && !loadedLoops.empty(),
              "the captured row's four columns are in the fixture");

        if (markedQuick.empty() || markedLoops.empty() || loadedQuick.empty() || loadedLoops.empty()) {
            std::cerr << "case 11b: no capture to compare with\n";
            return 1;
        }

        // What the capture itself says, so the comparison below is one
        // against a row that holds hot cues and a loop, moved alike.
        const QuickCuesBlob quickMarked = QuickCuesBlob::from_blob(markedQuick);
        const QuickCuesBlob quickLoaded = QuickCuesBlob::from_blob(loadedQuick);
        const LoopsBlob loopsMarked = LoopsBlob::from_blob(markedLoops);
        const LoopsBlob loopsLoaded = LoopsBlob::from_blob(loadedLoops);
        constexpr double kPlayerShift = 2292.0;
        check(quickMarked.quick_cues.size() == quickLoaded.quick_cues.size() && !loopsMarked.loops.empty()
                  && loopsMarked.loops.size() == loopsLoaded.loops.size(),
              "the capture holds the same slots before and after the load");
        if (failures > 0) {
            return 1;
        }
        int hotCues = 0;
        for (std::size_t i = 0; i < quickMarked.quick_cues.size(); ++i) {
            if (quickMarked.quick_cues[i].sample_offset >= 0.0) {
                ++hotCues;
                check(std::abs(quickMarked.quick_cues[i].sample_offset - quickLoaded.quick_cues[i].sample_offset
                               - kPlayerShift) < 1e-6,
                      "the player moved hot cue " + std::to_string(i + 1) + " 2292 samples earlier");
            }
        }
        check(hotCues == 3, "the captured row has three hot cues: " + std::to_string(hotCues));
        check(loopsMarked.loops[0].is_start_set && loopsMarked.loops[0].is_end_set
                  && std::abs(loopsMarked.loops[0].start_sample_offset - loopsLoaded.loops[0].start_sample_offset
                              - kPlayerShift) < 1e-6
                  && std::abs(loopsMarked.loops[0].end_sample_offset - loopsLoaded.loops[0].end_sample_offset
                              - kPlayerShift) < 1e-6,
              "the player moved both ends of the loop 2292 samples earlier, as the hot cues");
        check(quickMarked.adjusted_main_cue == 0.0 && quickLoaded.adjusted_main_cue == 0.0
                  && quickLoaded.default_main_cue == quickMarked.default_main_cue,
              "the player left the main cue, which was not set, alone");

        // The captured row onto a marked MP3 row of the fixture.
        constexpr std::int64_t kRow = 69;
        exec(copy.mdb, "UPDATE PerformanceData SET thirdPartySourceId = 1, quickCues = x'" + hexOf(markedQuick)
                           + "', loops = x'" + hexOf(markedLoops) + "' WHERE trackId = 69;");
        check(perf(copy.mdb, kRow, "quickCues") == markedQuick && perf(copy.mdb, kRow, "loops") == markedLoops,
              "the captured cues are on the row");

        // A lead-in made up to give the player's 2292 samples at 44.1 kHz:
        // rekordbox plays the tag frame (1152), Engine trims the delay and
        // 529 more (611 + 529). Not the captured file's own, which the
        // model gives 1105 (the fixture's README): this case holds how the
        // row is moved, not by how much.
        domain::Mp3LeadIn leadIn;
        leadIn.infoFrame = true;
        leadIn.tag = domain::Mp3LeadIn::Tag::Lame;
        leadIn.lameCrcValid = true;
        leadIn.encoderDelay = 611;
        leadIn.infoFrameModeMatchesAudio = true;
        leadIn.sampleRate = 44100;
        leadIn.samplesPerFrame = 1152;
        LibdjinteropEngineCueWriter writer(pathToUtf8(copy.root));
        writer.setLeadInSource([&](const std::string &) { return std::optional<domain::Mp3LeadIn>(leadIn); });
        const double shiftMs = writer.cueTimelineShiftMs(std::to_string(kRow));
        check(std::abs(shiftMs - kPlayerShift * 1000.0 / 44100) < 1e-6,
              "the row shifts by the player's 2292 samples: " + std::to_string(shiftMs) + " ms");

        RekordboxAnalysis analysis;
        analysis.beats = constantGrid(130.0, 480.0, 200000.0);
        analysis.threeBandPreview = synthetic(1200, 3, 12);
        analysis.scrollEntries = 30000;
        analysis.bpm = 130.0;
        AnalysisImportOptions options;
        options.fileSampleRateHz = 44100.0;
        const auto cued = writer.importAnalysis(std::to_string(kRow), analysis, options);
        check(cued.grid.outcome == Outcome::Written, "the captured row gets the grid");
        check(column(copy.mdb, "PerformanceData", "trackId", kRow, "thirdPartySourceId") == "NULL",
              "and loses the mark, as on the player");
        check(std::abs(TrackData::from_blob(perf(copy.mdb, kRow, "trackData")).sample_rate - 44100.0) < 1e-9,
              "the row counts at 44.1 kHz, the capture's rate");

        const QuickCuesBlob quickAfter = QuickCuesBlob::from_blob(perf(copy.mdb, kRow, "quickCues"));
        const LoopsBlob loopsAfter = LoopsBlob::from_blob(perf(copy.mdb, kRow, "loops"));
        bool asThePlayer = quickAfter.quick_cues.size() == quickLoaded.quick_cues.size()
            && loopsAfter.loops.size() == loopsLoaded.loops.size();
        for (std::size_t i = 0; asThePlayer && i < quickLoaded.quick_cues.size(); ++i) {
            const auto &want = quickLoaded.quick_cues[i];
            const auto &got = quickAfter.quick_cues[i];
            asThePlayer = std::abs(got.sample_offset - want.sample_offset) < 1e-6 && got.label == want.label
                && got.color == want.color;
            check(asThePlayer, "hot cue slot " + std::to_string(i + 1) + " as the player stored it: "
                                   + std::to_string(got.sample_offset) + " for " + std::to_string(want.sample_offset));
        }
        for (std::size_t i = 0; asThePlayer && i < loopsLoaded.loops.size(); ++i) {
            const auto &want = loopsLoaded.loops[i];
            const auto &got = loopsAfter.loops[i];
            asThePlayer = std::abs(got.start_sample_offset - want.start_sample_offset) < 1e-6
                && std::abs(got.end_sample_offset - want.end_sample_offset) < 1e-6 && got.is_start_set == want.is_start_set
                && got.is_end_set == want.is_end_set && got.label == want.label && got.color == want.color;
            check(asThePlayer, "loop slot " + std::to_string(i + 1) + " as the player stored it: "
                                   + std::to_string(got.start_sample_offset) + " to " + std::to_string(got.end_sample_offset));
        }
        check(asThePlayer && quickAfter.adjusted_main_cue == quickLoaded.adjusted_main_cue
                  && quickAfter.default_main_cue == quickLoaded.default_main_cue
                  && quickAfter.is_main_cue_adjusted == quickLoaded.is_main_cue_adjusted
                  && quickAfter.extra_data == quickLoaded.extra_data && loopsAfter.extra_data == loopsLoaded.extra_data,
              "the row's cues, loop and main cue are what the Prime 4 left");
        std::cout << "case 11b (a marked row moved as a Prime 4 moved it, loop included) done\n";
    }

    // 12. writeHotCues() takes the mark off the row it writes when the
    // shift it took off is known: every cue there is on Engine's timeline
    // then. Known: an MP3 whose lead-in reads, or a file that is not an
    // MP3 (0). Not known (an MP3 whose lead-in does not read, or with the
    // correction off): the cues are written as before and the mark stays,
    // for the player to move them. A row without the mark is left without
    // one, and no other row's mark changes. On 3.x in PerformanceData, on
    // 2.18 in Track.
    {
        const Copy copy = freshCopy("rekordbox-import-mark-cues");
        const auto mark = [&](std::int64_t id) {
            return column(copy.mdb, "PerformanceData", "trackId", id, "thirdPartySourceId");
        };
        constexpr std::int64_t kMarked = 17;
        constexpr std::int64_t kUnmarked = 30;
        check(mark(kMarked) == "1" && mark(kUnmarked) == "NULL", "the fixture's marks for 17 and 30");
        constexpr std::int64_t kMarkedCorrectionOff = 69;
        constexpr std::int64_t kMarkedNotAnMp3 = 377;
        exec(copy.mdb, "UPDATE PerformanceData SET thirdPartySourceId = 1 WHERE trackId = 377;"
                       "UPDATE Track SET path = replace(path, '.mp3', '.m4a') WHERE id = 377;");
        const auto marks = [&] {
            auto all = rows(copy.mdb, "SELECT trackId, quote(thirdPartySourceId) FROM PerformanceData");
            all.erase(kMarked);
            all.erase(kMarkedNotAnMp3);
            return all;
        };
        const auto othersBefore = marks();
        domain::CuePoint hot;
        hot.kind = domain::CuePoint::Kind::Hot;
        hot.hotCueNumber = 1;
        hot.positionMs = 5000.0;
        const auto leadIn =
            infrastructure::audio::readMp3LeadIn(pathToUtf8(g_fixtures / "mp3-lead-in" / "lame-cbr-44100.mp3"));
        check(leadIn.has_value(), "the LAME fixture's lead-in reads");
        LibdjinteropEngineCueWriter shiftedWriter(pathToUtf8(copy.root));
        shiftedWriter.setLeadInSource([&](const std::string &) { return leadIn; });
        auto writer = unshiftedWriter(pathToUtf8(copy.root));

        writer.writeHotCues(std::to_string(kMarked), {hot});
        check(mark(kMarked) == "1", "3.x: an MP3 whose lead-in does not read keeps the mark: " + mark(kMarked));
        {
            auto db = djinterop::engine::load_database(pathToUtf8(copy.root));
            const auto written = db.track_by_id(kMarked)->hot_cues().at(0);
            const auto rate = db.track_by_id(kMarked)->sample_rate().value_or(44100.0);
            check(written && std::abs(written->sample_offset - 5000.0 / 1000.0 * rate) < 1e-6,
                  "3.x: and its cue is written where it was given, as before");
        }
        domain::MatchingPolicy::setCorrectMp3CueTiming(false);
        shiftedWriter.writeHotCues(std::to_string(kMarkedCorrectionOff), {hot});
        domain::MatchingPolicy::setCorrectMp3CueTiming(true);
        check(mark(kMarkedCorrectionOff) == "1",
              "3.x: an MP3 written with the correction off keeps the mark: " + mark(kMarkedCorrectionOff));
        shiftedWriter.writeHotCues(std::to_string(kMarked), {hot});
        check(mark(kMarked) == "NULL", "3.x: an MP3 whose lead-in reads loses the mark: " + mark(kMarked));
        writer.writeHotCues(std::to_string(kMarkedNotAnMp3), {hot});
        check(mark(kMarkedNotAnMp3) == "NULL",
              "3.x: a file that is not an MP3 (shift 0, known) loses the mark: " + mark(kMarkedNotAnMp3));
        shiftedWriter.writeHotCues(std::to_string(kUnmarked), {hot});
        check(mark(kUnmarked) == "NULL", "3.x: an unmarked row stays without: " + mark(kUnmarked));
        check(marks() == othersBefore, "3.x: no other row's mark changed");

        // Only 1 is the mark: a row holding another value keeps it through
        // a cue write and through a grid, with cues or without.
        constexpr std::int64_t kOtherValueWithCues = 69;
        constexpr std::int64_t kOtherValueNoCues = kAnalysed;
        exec(copy.mdb, "UPDATE PerformanceData SET thirdPartySourceId = 2 WHERE trackId IN (69, 6, 618);");
        shiftedWriter.writeHotCues("618", {hot});
        check(mark(618) == "2", "3.x: writeHotCues leaves thirdPartySourceId 2: " + mark(618));
        {
            LibdjinteropEngineCueWriter shifted(pathToUtf8(copy.root));
            shifted.setLeadInSource([&](const std::string &) { return leadIn; });
            RekordboxAnalysis analysis;
            analysis.beats = constantGrid(120.0, 480.0, 200000.0);
            analysis.threeBandPreview = synthetic(1200, 3, 13);
            analysis.scrollEntries = 30000;
            AnalysisImportOptions options;
            options.fileSampleRateHz = 44100.0;
            const auto quickCuesBefore = perf(copy.mdb, kOtherValueWithCues, "quickCues");
            const auto loopsBefore = perf(copy.mdb, kOtherValueWithCues, "loops");
            check(shifted.importAnalysis(std::to_string(kOtherValueWithCues), analysis, options).grid.outcome
                      == Outcome::Written,
                  "3.x: a row with cues and thirdPartySourceId 2 gets the grid");
            check(mark(kOtherValueWithCues) == "2" && perf(copy.mdb, kOtherValueWithCues, "quickCues") == quickCuesBefore
                      && perf(copy.mdb, kOtherValueWithCues, "loops") == loopsBefore,
                  "3.x: and keeps its cues and the 2: " + mark(kOtherValueWithCues));
            check(shifted.writeBeatGrid(std::to_string(kOtherValueNoCues), analysis.beats).outcome == Outcome::Written,
                  "3.x: a row without cues and thirdPartySourceId 2 gets the grid");
            check(mark(kOtherValueNoCues) == "2", "3.x: and keeps the 2: " + mark(kOtherValueNoCues));
        }

        const fs::path dir = testing::scratchRoot() / "engine_analysis_import" / "schema-2-cues";
        fs::remove_all(dir);
        fs::create_directories(dir);
        const std::string root = pathToUtf8(dir / "Engine Library");
        std::int64_t markedId = 0;
        std::int64_t unmarkedId = 0;
        {
            auto db = djinterop::engine::create_database(root, djinterop::engine::engine_schema::schema_2_18_0);
            djinterop::track_snapshot snapshot;
            snapshot.title = "Subject";
            // Not MP3s: a shift of 0, known, so the unshifted writer
            // takes the mark off.
            snapshot.relative_path = "subject.wav";
            snapshot.sample_rate = 44100.0;
            markedId = db.create_track(snapshot).id();
            snapshot.relative_path = "other.wav";
            unmarkedId = db.create_track(snapshot).id();
        }
        const std::string mdb = pathToUtf8(dir / "Engine Library" / "Database2" / "m.db");
        exec(mdb, "UPDATE Track SET thirdPartySourceId = 1 WHERE id = " + std::to_string(markedId));
        const auto mark2 = [&](std::int64_t id) { return column(mdb, "Track", "id", id, "thirdPartySourceId"); };
        check(mark2(markedId) == "1" && mark2(unmarkedId) == "NULL", "2.18: one row marked");
        auto writer2 = unshiftedWriter(root);
        writer2.writeHotCues(std::to_string(markedId), {hot});
        check(mark2(markedId) == "NULL", "2.18: a marked row loses the mark with its cues: " + mark2(markedId));
        writer2.writeHotCues(std::to_string(unmarkedId), {hot});
        check(mark2(unmarkedId) == "NULL", "2.18: an unmarked row stays without: " + mark2(unmarkedId));
        exec(mdb, "UPDATE Track SET thirdPartySourceId = 2 WHERE id = " + std::to_string(unmarkedId));
        writer2.writeHotCues(std::to_string(unmarkedId), {hot});
        check(mark2(unmarkedId) == "2", "2.18: writeHotCues leaves thirdPartySourceId 2: " + mark2(unmarkedId));
        std::cout << "case 12 (writeHotCues takes the mark off) done\n";
    }

    if (failures == 0) {
        std::cout << "libdjinterop_engine_analysis_import_test: all cases passed\n";
    }
    return failures == 0 ? 0 : 1;
}
