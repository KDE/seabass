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
//
// usage: libdjinterop_engine_analysis_import_test <tests/fixtures>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
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
#include <djinterop/engine/v3/overview_waveform_data_blob.hpp>
#include <djinterop/engine/v3/track_data_blob.hpp>
#include <sqlite3.h>
#include <zlib.h>

#include "domain/beat_grid.hpp"
#include "domain/engine_overview.hpp"
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

    // 1. An analysed row: the waveform replaced, the grid as writeBeatGrid()
    // writes it, everything else as it was; then AlreadySame.
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

        auto writer = unshiftedWriter(pathToUtf8(copy.root));
        const auto result = writer.importAnalysis(std::to_string(kAnalysed), analysis);
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
        const auto same = again.importAnalysis(std::to_string(kAnalysed), analysis);
        check(same.grid.outcome == Outcome::AlreadySame && same.waveform == Waveform::AlreadySame,
              "the same call again is AlreadySame for both parts");
        check(same.overviewFileRelativePath.empty(), "and names no file written");
        check(hookCalls == 0, "and calls no hook");
        check(column(copy.mdb, "Track", "id", kAnalysed, "lastEditTime") == "12345", "the row's edit time stays");
        check(fs::last_write_time(copy.root / fileOf(kAnalysed)) == fileTime, "the file's time stays");
        check(fileBytes(pathFromUtf8(copy.mdb)) == dbBytes, "m.db is byte for byte as it was");
        std::cout << "case 1 (analysed row) done\n";
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

        RekordboxAnalysis analysis;
        analysis.beats = constantGrid(250.0, 480.0, 120000.0);
        analysis.threeBandPreview = synthetic(1200, 3, 9);
        analysis.scrollEntries = 18000;
        analysis.bpm = 125.0;
        AnalysisImportOptions options;
        options.fileSampleRateHz = 44100.0;
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
        auto writer = unshiftedWriter(pathToUtf8(copy.root));
        check(writer.importAnalysis(std::to_string(id), analysis, waveformOnly).waveform == Waveform::Written,
              "ours written over it");
        const std::string ours = fileBytes(copy.root / fileOf(id));
        check(ours.substr(0, 4) == real.substr(0, 4) && inflated(asBytes(ours)).size() == 3099,
              "ours has the same header and raw length");
        std::cout << "case 9 (overview file container) done\n";
    }

    if (failures == 0) {
        std::cout << "libdjinterop_engine_analysis_import_test: all cases passed\n";
    }
    return failures == 0 ? 0 : 1;
}
