// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// LibdjinteropEngineCueWriter::writeBeatGrid() puts a rekordbox beat grid
// into an Engine row's adjusted grid. Over scratch copies of the committed
// anonymized fixture's Engine library (schema 3.0.2, written by Denon),
// one copy per case:
//
// - an analysed row given a constant tempo in whole milliseconds, as
//   rekordbox stores it: Written; read back through the project's own
//   reader every source beat is there within 1 ms on the same place in
//   the bar; the default grid, rate, length and trailing bytes are byte
//   for byte as before; the adjusted grid is numbered as Denon numbers
//   its own (first marker -4 before the start, the last at or past the
//   end, each counting the beats to the next); the row's flags and tempo
//   are untouched and so is every other row's beat data;
// - the same write again: AlreadySame, and the row's edit time stays;
// - a tempo change: more than two markers, and the same round trip;
// - a row the player has not analysed: Skip writes nothing, Fill without
//   the file's rate is refused, Fill with it makes a grid that reads
//   back and leaves the row unanalysed and unlocked, FillAndLock locks;
// - a locked row and a row whose grid was adjusted on the player are
//   refused, and written when told to overwrite, the default grid kept;
// - with an MP3 lead-in shift, a hot cue written at a source beat's time
//   and that beat of the written grid land on the same sample;
// - no beats or one beat: NoSourceGrid, nothing written;
// - a preview (previewOnly) answers every one of those as the real call
//   does, fingerprint included, writing nothing and calling no hook.
//
// usage: libdjinterop_engine_beat_grid_writer_test <tests/fixtures>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <djinterop/djinterop.hpp>
#include <djinterop/engine/v3/beat_data_blob.hpp>
#include <djinterop/engine/v2/engine_library.hpp>
#include <djinterop/engine/v3/engine_library.hpp>
#include <sqlite3.h>

#include "domain/beat_grid.hpp"
#include "domain/matching_policy.hpp"
#include "domain/track.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/engine/libdjinterop_beat_grid_reader.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using namespace seabass::infrastructure::engine;
using Outcome = BeatGridWriteResult::Outcome;
using BeatData = djinterop::engine::v3::beat_data_blob;

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

// The analysed rows used here, all MP3s with a Denon grid; and two the
// player has not analysed (no beat data at all).
constexpr std::int64_t kConstant = 6;
constexpr std::int64_t kTempoChange = 7;
constexpr std::int64_t kLocked = 8;
constexpr std::int64_t kEdited = 10;
constexpr std::int64_t kShifted = 14;
constexpr std::int64_t kUnanalysed = 1;
constexpr std::int64_t kUnanalysedToo = 2;

struct Copy
{
    fs::path root;  // the "Engine Library" folder
    std::string mdb;
};

// A copy, never the fixture itself: opening its m.db in place could
// change it.
Copy freshCopy(const std::string &caseName)
{
    const fs::path dir = testing::scratchRoot() / "engine_beat_grid_writer" / pathFromUtf8(caseName);
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

// Every row's beatData, NULL as nullopt.
std::map<std::int64_t, std::optional<std::vector<unsigned char>>> allBeatData(const std::string &mdb)
{
    sqlite3 *handle = open(mdb, SQLITE_OPEN_READONLY);
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(handle, "SELECT trackId, beatData FROM PerformanceData", -1, &stmt, nullptr);
    std::map<std::int64_t, std::optional<std::vector<unsigned char>>> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const std::int64_t id = sqlite3_column_int64(stmt, 0);
        if (sqlite3_column_type(stmt, 1) == SQLITE_NULL) {
            out[id] = std::nullopt;
        } else {
            const auto *bytes = static_cast<const unsigned char *>(sqlite3_column_blob(stmt, 1));
            out[id] = std::vector<unsigned char>(bytes, bytes + sqlite3_column_bytes(stmt, 1));
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);
    return out;
}

std::optional<BeatData> beatDataOf(const std::string &mdb, std::int64_t id)
{
    const auto raw = allBeatData(mdb).at(id);
    if (!raw) {
        return std::nullopt;
    }
    std::vector<std::byte> bytes;
    for (unsigned char c : *raw) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    return BeatData::from_blob(bytes);
}

// One column of the Track row, as text so NULL and REAL say what they are.
std::string trackColumn(const std::string &mdb, std::int64_t id, const std::string &column)
{
    sqlite3 *handle = open(mdb, SQLITE_OPEN_READONLY);
    sqlite3_stmt *stmt = nullptr;
    const std::string sql = "SELECT quote(" + column + ") FROM Track WHERE id = " + std::to_string(id);
    const int prepared = sqlite3_prepare_v2(handle, sql.c_str(), -1, &stmt, nullptr);
    assert(prepared == SQLITE_OK);
    (void)prepared;
    std::string value = "<no row>";
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);
    return value;
}

void exec(const std::string &mdb, const std::string &sql)
{
    sqlite3 *handle = open(mdb, SQLITE_OPEN_READWRITE);
    const int done = sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, nullptr);
    assert(done == SQLITE_OK);
    (void)done;
    sqlite3_close(handle);
}

// Writers that never read a file for its lead-in: the fixture has no
// audio, and every case but one wants no shift.
LibdjinteropEngineCueWriter unshiftedWriter(const Copy &copy)
{
    LibdjinteropEngineCueWriter writer(pathToUtf8(copy.root));
    writer.setLeadInSource([](const std::string &) { return std::optional<domain::Mp3LeadIn>(); });
    return writer;
}

// rekordbox's grid: every beat, in whole milliseconds, counting the bar.
std::vector<domain::Beat> constantGrid(double firstMs, double beatMs, double untilMs, int firstBeatInBar)
{
    std::vector<domain::Beat> beats;
    for (int i = 0; firstMs + i * beatMs < untilMs; ++i) {
        beats.push_back({std::round(firstMs + i * beatMs), (firstBeatInBar - 1 + i) % 4 + 1});
    }
    return beats;
}

// Every source beat inside the track read back within a millisecond, on
// the same place in the bar.
bool readsBack(const Copy &copy, std::int64_t id, const std::vector<domain::Beat> &source, const std::string &who)
{
    const auto beats = readBeatGrid(pathToUtf8(copy.root), std::to_string(id));
    std::size_t at = 0;
    for (const domain::Beat &want : source) {
        while (at < beats.size() && beats[at].timeMs < want.timeMs - 1.0) {
            ++at;
        }
        if (at == beats.size() || std::abs(beats[at].timeMs - want.timeMs) > 1.0 || beats[at].beatInBar != want.beatInBar) {
            check(false, who + ": source beat at " + std::to_string(want.timeMs) + " (" + std::to_string(want.beatInBar)
                             + ") did not come back");
            return false;
        }
    }
    return true;
}

// The adjusted grid is numbered as Denon numbers its own.
bool denonShaped(const std::vector<djinterop::engine::v3::beat_grid_marker_blob> &grid, double samples)
{
    if (grid.size() < 2 || grid.front().beat_number != -4 || !(grid.front().sample_offset < 0.0)
        || !(grid.back().sample_offset >= samples) || grid.back().number_of_beats != 0) {
        return false;
    }
    for (std::size_t i = 0; i + 1 < grid.size(); ++i) {
        if (grid[i].number_of_beats != grid[i + 1].beat_number - grid[i].beat_number || grid[i].unknown_value_1 != 0) {
            return false;
        }
    }
    return true;
}

// Where beat `index` of a grid is, in samples.
double sampleOfBeat(const std::vector<djinterop::engine::v3::beat_grid_marker_blob> &grid, std::int64_t index)
{
    for (std::size_t i = 0; i + 1 < grid.size(); ++i) {
        const auto &a = grid[i];
        const auto &b = grid[i + 1];
        if (index >= a.beat_number && index <= b.beat_number) {
            return a.sample_offset
                + (b.sample_offset - a.sample_offset) * static_cast<double>(index - a.beat_number)
                / static_cast<double>(b.beat_number - a.beat_number);
        }
    }
    return std::nan("");
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: libdjinterop_engine_beat_grid_writer_test <tests/fixtures>\n";
        return 2;
    }
    g_fixtures = pathFromUtf8(argv[1]);
    domain::MatchingPolicy::setCorrectMp3CueTiming(true);

    // 1. An analysed row, a constant tempo. 2. The same write again.
    {
        const Copy copy = freshCopy("constant");
        const auto before = allBeatData(copy.mdb);
        const BeatData old = *beatDataOf(copy.mdb, kConstant);
        check(old.sample_rate > 0.0 && old.samples > 0.0 && old.default_beat_grid == old.adjusted_beat_grid,
              "the fixture's row has Denon's own grid");
        const double durationMs = old.samples / old.sample_rate * 1000.0;
        const std::vector<std::string> kept = {"isAnalyzed", "bpm", "bpmAnalyzed", "isBeatGridLocked"};
        std::map<std::string, std::string> flagsBefore;
        for (const auto &column : kept) {
            flagsBefore[column] = trackColumn(copy.mdb, kConstant, column);
        }
        exec(copy.mdb, "UPDATE Track SET lastEditTime = 1000 WHERE id = " + std::to_string(kConstant));

        // 121 BPM from 37 ms, the first beat the third of its bar.
        const auto source = constantGrid(37.0, 60000.0 / 121.0, durationMs, 3);
        auto writer = unshiftedWriter(copy);
        const auto result = writer.writeBeatGrid(std::to_string(kConstant), source);
        check(result.outcome == Outcome::Written, "an analysed row is written");
        check(result.markers == 2 && result.barBreaks == 0, "a constant tempo is two markers, no bar breaks");

        const BeatData now = *beatDataOf(copy.mdb, kConstant);
        check(now.default_beat_grid == old.default_beat_grid, "the default grid is Engine's still");
        check(now.sample_rate == old.sample_rate && now.samples == old.samples, "and the rate and length too");
        check(now.extra_data == old.extra_data, "and the bytes after the grids");
        check(now.is_beatgrid_set == 1, "the grid is set");
        check(denonShaped(now.adjusted_beat_grid, now.samples), "the adjusted grid is numbered as Denon numbers its own");
        check(now.adjusted_beat_grid != old.adjusted_beat_grid, "and is the new one");
        readsBack(copy, kConstant, source, "constant tempo");
        for (const auto &column : kept) {
            check(trackColumn(copy.mdb, kConstant, column) == flagsBefore[column], column + " is as it was");
        }
        check(trackColumn(copy.mdb, kConstant, "lastEditTime") != "1000", "a write is an edit of the row");
        const auto after = allBeatData(copy.mdb);
        int othersChanged = 0;
        for (const auto &[id, blob] : before) {
            if (id != kConstant && after.at(id) != blob) {
                ++othersChanged;
            }
        }
        check(othersChanged == 0, "no other row's beat data changed, " + std::to_string(othersChanged) + " did");

        exec(copy.mdb, "UPDATE Track SET lastEditTime = 1000 WHERE id = " + std::to_string(kConstant));
        const auto bytesBefore = allBeatData(copy.mdb).at(kConstant);
        auto again = unshiftedWriter(copy);
        const auto second = again.writeBeatGrid(std::to_string(kConstant), source);
        check(second.outcome == Outcome::AlreadySame, "the same grid again is AlreadySame");
        check(trackColumn(copy.mdb, kConstant, "lastEditTime") == "1000", "and is not written: the edit time stays");
        check(allBeatData(copy.mdb).at(kConstant) == bytesBefore, "nor the blob");
        std::cout << "cases 1, 2 (constant tempo; again is AlreadySame) done\n";
    }

    // 3. A tempo change.
    {
        const Copy copy = freshCopy("tempo-change");
        const BeatData old = *beatDataOf(copy.mdb, kTempoChange);
        const double durationMs = old.samples / old.sample_rate * 1000.0;
        std::vector<domain::Beat> source;
        double t = 412.0;
        for (int i = 0; t < durationMs; ++i) {
            source.push_back({std::round(t), i % 4 + 1});
            t += i < 199 ? 500.0 : 60000.0 / 126.0;
        }
        auto writer = unshiftedWriter(copy);
        const auto result = writer.writeBeatGrid(std::to_string(kTempoChange), source);
        check(result.outcome == Outcome::Written && result.markers > 2, "a tempo change is written as more than two markers");
        const BeatData now = *beatDataOf(copy.mdb, kTempoChange);
        check(now.adjusted_beat_grid.size() > 2 && denonShaped(now.adjusted_beat_grid, now.samples),
              "numbered as Denon numbers its own");
        check(now.default_beat_grid == old.default_beat_grid, "the default grid kept");
        readsBack(copy, kTempoChange, source, "tempo change");
        std::cout << "case 3 (tempo change) done\n";
    }

    // 4. Rows the player has not analysed.
    {
        const Copy copy = freshCopy("unanalysed");
        check(!allBeatData(copy.mdb).at(kUnanalysed) && !allBeatData(copy.mdb).at(kUnanalysedToo),
              "the fixture's unanalysed rows have no beat data");
        const auto source = constantGrid(120.0, 480.0, 200000.0, 1);
        const auto before = allBeatData(copy.mdb);
        // What the writer goes by: libdjinterop reads a NULL beatData as
        // an empty blob, all zeros, rather than throwing.
        {
            auto library = djinterop::engine::v3::engine_library::load(pathToUtf8(copy.root));
            const BeatData empty = library.performance_data().get_beat_data(kUnanalysed);
            check(empty.sample_rate == 0.0 && empty.samples == 0.0 && empty.is_beatgrid_set == 0
                      && empty.default_beat_grid.empty() && empty.adjusted_beat_grid.empty() && empty.extra_data.empty(),
                  "a NULL beatData reads as an empty blob");
        }

        auto writer = unshiftedWriter(copy);
        check(writer.writeBeatGrid(std::to_string(kUnanalysed), source).outcome == Outcome::RowNotAnalysed,
              "Skip, the default, refuses an unanalysed row");
        BeatGridWriteOptions fill;
        fill.unanalysed = UnanalysedRowGrid::Fill;
        check(writer.writeBeatGrid(std::to_string(kUnanalysed), source, fill).outcome == Outcome::NoSampleRate,
              "Fill without the file's rate is refused");
        check(allBeatData(copy.mdb) == before, "and neither wrote anything");

        fill.fileSampleRateHz = 48000.0;
        fill.fileDurationMs = 200000.0;
        const auto filled = writer.writeBeatGrid(std::to_string(kUnanalysed), source, fill);
        check(filled.outcome == Outcome::Written, "Fill with the rate and length writes");
        const auto blob = beatDataOf(copy.mdb, kUnanalysed);
        check(blob && blob->sample_rate == 48000.0 && blob->samples == 9600000.0 && blob->is_beatgrid_set == 1,
              "a blob at the file's rate and length");
        check(blob && blob->default_beat_grid == blob->adjusted_beat_grid && denonShaped(blob->adjusted_beat_grid, blob->samples),
              "both grids the converted one, numbered as Denon's");
        check(blob && blob->extra_data == std::vector<std::byte>(9, std::byte{0}), "nine zero bytes after them");
        check(trackColumn(copy.mdb, kUnanalysed, "isAnalyzed") == "0", "the row is still unanalysed");
        check(trackColumn(copy.mdb, kUnanalysed, "isBeatGridLocked") == "0", "and not locked");
        // The reader takes the length from the Track row, which an
        // unanalysed row does not have: the grid comes back unbounded at
        // the end, which is fine for this check.
        readsBack(copy, kUnanalysed, source, "filled row");

        BeatGridWriteOptions lock = fill;
        lock.unanalysed = UnanalysedRowGrid::FillAndLock;
        check(writer.writeBeatGrid(std::to_string(kUnanalysedToo), source, lock).outcome == Outcome::Written,
              "FillAndLock writes");
        check(trackColumn(copy.mdb, kUnanalysedToo, "isBeatGridLocked") == "1", "and locks the grid");
        check(trackColumn(copy.mdb, kUnanalysedToo, "isAnalyzed") == "0", "leaving the row unanalysed");
        check(beatDataOf(copy.mdb, kUnanalysedToo).has_value(), "with its grid");
        // A lock is the user's word on an unanalysed row too. The same
        // grid again is still the same, lock and all; another grid is
        // refused, with either Fill mode, until the caller says to write
        // over what was set on the player.
        check(writer.writeBeatGrid(std::to_string(kUnanalysedToo), source, lock).outcome == Outcome::AlreadySame,
              "FillAndLock again is the same, not refused by its own lock");
        const auto other = constantGrid(126.0, 480.0, 200000.0, 1);
        const auto lockedBefore = allBeatData(copy.mdb);
        check(writer.writeBeatGrid(std::to_string(kUnanalysedToo), other, lock).outcome == Outcome::RowLocked,
              "another grid on a locked unanalysed row is refused");
        check(writer.writeBeatGrid(std::to_string(kUnanalysedToo), other, fill).outcome == Outcome::RowLocked,
              "by Fill as well");
        check(allBeatData(copy.mdb) == lockedBefore, "and nothing was written");
        BeatGridWriteOptions over = lock;
        over.overwriteEngineEdits = true;
        check(writer.writeBeatGrid(std::to_string(kUnanalysedToo), other, over).outcome == Outcome::Written,
              "and written when the caller says to write over it");
        std::cout << "case 4 (unanalysed rows) done\n";
    }

    // 5. Grids the user set on the player.
    {
        const Copy copy = freshCopy("engine-edits");
        exec(copy.mdb, "UPDATE Track SET isBeatGridLocked = 1 WHERE id = " + std::to_string(kLocked));
        BeatData edited = *beatDataOf(copy.mdb, kEdited);
        for (auto &marker : edited.adjusted_beat_grid) {
            marker.sample_offset += 441.0;
        }
        {
            auto db = djinterop::engine::v3::engine_library::load(pathToUtf8(copy.root));
            db.performance_data().set_beat_data(kEdited, edited);
        }
        const auto before = allBeatData(copy.mdb);
        const auto lockedOld = *beatDataOf(copy.mdb, kLocked);
        const double lockedMs = lockedOld.samples / lockedOld.sample_rate * 1000.0;
        const double editedMs = edited.samples / edited.sample_rate * 1000.0;

        auto writer = unshiftedWriter(copy);
        check(writer.writeBeatGrid(std::to_string(kLocked), constantGrid(50.0, 451.0, lockedMs, 1)).outcome == Outcome::RowLocked,
              "a locked row is refused");
        check(writer.writeBeatGrid(std::to_string(kEdited), constantGrid(50.0, 488.0, editedMs, 1)).outcome
                  == Outcome::RowEditedOnEngine,
              "a row adjusted on the player is refused");
        check(allBeatData(copy.mdb) == before, "and neither is written");

        BeatGridWriteOptions overwrite;
        overwrite.overwriteEngineEdits = true;
        check(writer.writeBeatGrid(std::to_string(kLocked), constantGrid(50.0, 451.0, lockedMs, 1), overwrite).outcome
                  == Outcome::Written,
              "a locked row is written when told to");
        check(writer.writeBeatGrid(std::to_string(kEdited), constantGrid(50.0, 488.0, editedMs, 1), overwrite).outcome
                  == Outcome::Written,
              "and an adjusted one");
        check(beatDataOf(copy.mdb, kLocked)->default_beat_grid == lockedOld.default_beat_grid
                  && beatDataOf(copy.mdb, kEdited)->default_beat_grid == edited.default_beat_grid,
              "their default grids kept");
        check(trackColumn(copy.mdb, kLocked, "isBeatGridLocked") == "1", "the lock stays");
        std::cout << "case 5 (locked and adjusted rows) done\n";
    }

    // 6. The same time base as the cues. The writer is told the row's MP3
    // has a LAME lead-in; a hot cue at a source beat's rekordbox time and
    // that beat of the grid must land on one sample. The beats are exact
    // here, 500 ms apart, so the grid's line runs through every one of
    // them and any difference is the time base.
    {
        const Copy copy = freshCopy("shift");
        const auto leadIn = infrastructure::audio::readMp3LeadIn(
            pathToUtf8(g_fixtures / "mp3-lead-in" / "lame-cbr-44100.mp3"));
        check(leadIn.has_value(), "the LAME fixture's lead-in reads");
        LibdjinteropEngineCueWriter writer(pathToUtf8(copy.root));
        writer.setLeadInSource([&](const std::string &) { return leadIn; });
        const double shiftMs = writer.cueTimelineShiftMs(std::to_string(kShifted));
        check(shiftMs > 10.0, "the row's cues shift, by " + std::to_string(shiftMs) + " ms");

        const BeatData old = *beatDataOf(copy.mdb, kShifted);
        const double durationMs = old.samples / old.sample_rate * 1000.0;
        const auto source = constantGrid(1000.0, 500.0, durationMs, 1);
        check(writer.writeBeatGrid(std::to_string(kShifted), source).outcome == Outcome::Written, "the shifted grid is written");

        constexpr int k = 40;
        domain::CuePoint cue;
        cue.kind = domain::CuePoint::Kind::Hot;
        cue.hotCueNumber = 1;
        cue.positionMs = source[k].timeMs;
        writer.writeHotCues(std::to_string(kShifted), {cue});

        auto db = djinterop::engine::load_database(pathToUtf8(copy.root));
        const auto cues = db.track_by_id(kShifted)->hot_cues();
        check(!cues.empty() && cues[0].has_value(), "the hot cue is there");
        const BeatData now = *beatDataOf(copy.mdb, kShifted);
        // Source beat 0 is a downbeat 1000 ms in: on Engine's timeline the
        // first downbeat at or after the start, index 0.
        const double beatSample = sampleOfBeat(now.adjusted_beat_grid, k);
        if (!cues.empty() && cues[0]) {
            check(std::abs(cues[0]->sample_offset - beatSample) <= 1.0,
                  "the cue at beat " + std::to_string(k) + " is at sample " + std::to_string(cues[0]->sample_offset)
                      + ", the grid's beat at " + std::to_string(beatSample));
        }
        std::cout << "case 6 (one time base with the cues) done\n";
    }

    // 7. No grid to write.
    {
        const Copy copy = freshCopy("no-source");
        const auto before = allBeatData(copy.mdb);
        auto writer = unshiftedWriter(copy);
        check(writer.writeBeatGrid(std::to_string(kConstant), {}).outcome == Outcome::NoSourceGrid, "no beats is no grid");
        check(writer.writeBeatGrid(std::to_string(kConstant), {{1000.0, 1}}).outcome == Outcome::NoSourceGrid,
              "nor is one beat");
        check(allBeatData(copy.mdb) == before, "and nothing is written");
        bool threw = false;
        try {
            writer.writeBeatGrid("4000000", constantGrid(0.0, 500.0, 10000.0, 1));
        } catch (const std::exception &) {
            threw = true;
        }
        check(threw, "a track the library has not throws");
        std::cout << "case 7 (no source grid) done\n";
    }

    // 8. Engine 2.x keeps beat data in the Track table: the same write
    // there, on a fresh 2.18 library with one analysed row, and a refusal
    // on a 1.x one.
    {
        const fs::path dir = testing::scratchRoot() / "engine_beat_grid_writer" / "schema-2";
        fs::remove_all(dir);
        fs::create_directories(dir);
        const std::string root = pathToUtf8(dir / "Engine Library");
        std::int64_t id = 0;
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
            id = db.create_track(snapshot).id();
        }
        auto library = djinterop::engine::v2::engine_library::load(root);
        const BeatData old = library.track().get_beat_data(id);
        check(old.sample_rate == 44100.0 && old.default_beat_grid.size() == 2, "the 2.18 row has a grid");
        LibdjinteropEngineCueWriter writer(root);
        writer.setLeadInSource([](const std::string &) { return std::optional<domain::Mp3LeadIn>(); });
        const auto source = constantGrid(250.0, 480.0, 120000.0, 1);
        const auto result = writer.writeBeatGrid(std::to_string(id), source);
        check(result.outcome == Outcome::Written, "a 2.18 row is written");
        const BeatData now = library.track().get_beat_data(id);
        check(now.default_beat_grid == old.default_beat_grid, "its default grid kept");
        check(denonShaped(now.adjusted_beat_grid, now.samples), "its adjusted one numbered as Denon's");
        check(now.adjusted_beat_grid != old.adjusted_beat_grid, "and the new one");

        const fs::path dir1 = testing::scratchRoot() / "engine_beat_grid_writer" / "schema-1";
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
        check(writer1.writeBeatGrid(std::to_string(id1), source).outcome == Outcome::UnsupportedSchema,
              "Engine 1.x is not written");
        std::cout << "case 8 (schema 2.18 and 1.x) done\n";
    }

    // 9. Seabass's own earlier write is not an edit on the player. A grid
    // written here, then corrected in rekordbox and written again: with
    // the first write's fingerprint the second goes through; without it,
    // with a wrong one, or once the player moved the grid, it is refused;
    // and a lock still refuses whatever the fingerprint says.
    {
        const Copy copy = freshCopy("own-grid");
        const BeatData denon = *beatDataOf(copy.mdb, kConstant);
        const double durationMs = denon.samples / denon.sample_rate * 1000.0;
        const auto g1 = constantGrid(37.0, 60000.0 / 121.0, durationMs, 1);
        const auto g2 = constantGrid(41.0, 60000.0 / 121.0, durationMs, 1);
        const auto g3 = constantGrid(45.0, 60000.0 / 120.0, durationMs, 1);
        const auto g4 = constantGrid(49.0, 60000.0 / 122.0, durationMs, 1);
        const std::string id = std::to_string(kConstant);
        auto writer = unshiftedWriter(copy);

        const auto first = writer.writeBeatGrid(id, g1);
        check(first.outcome == Outcome::Written, "G1 is written");
        check(first.gridFingerprint.size() == 64, "with a fingerprint");
        const auto again = writer.writeBeatGrid(id, g1);
        check(again.outcome == Outcome::AlreadySame && again.gridFingerprint == first.gridFingerprint,
              "G1 again is AlreadySame, with the same fingerprint");

        const auto before = allBeatData(copy.mdb);
        const auto noFingerprint = writer.writeBeatGrid(id, g2);
        check(noFingerprint.outcome == Outcome::RowEditedOnEngine, "G2 without a fingerprint is refused as edited");
        check(noFingerprint.gridFingerprint.empty(), "and a refusal has no fingerprint");
        check(allBeatData(copy.mdb) == before, "nothing written");

        BeatGridWriteOptions own;
        own.ownGridFingerprint = first.gridFingerprint;
        const auto second = writer.writeBeatGrid(id, g2, own);
        check(second.outcome == Outcome::Written, "G2 with G1's fingerprint is written");
        check(!second.gridFingerprint.empty() && second.gridFingerprint != first.gridFingerprint,
              "with a fingerprint of its own");
        check(beatDataOf(copy.mdb, kConstant)->default_beat_grid == denon.default_beat_grid, "the default grid is Denon's still");
        readsBack(copy, kConstant, g2, "own grid rewritten");

        // The player moves the grid: the fingerprint no longer fits.
        BeatData moved = *beatDataOf(copy.mdb, kConstant);
        for (auto &marker : moved.adjusted_beat_grid) {
            marker.sample_offset += 441.0;
        }
        {
            auto db = djinterop::engine::v3::engine_library::load(pathToUtf8(copy.root));
            db.performance_data().set_beat_data(kConstant, moved);
        }
        const auto movedBytes = allBeatData(copy.mdb);
        own.ownGridFingerprint = second.gridFingerprint;
        check(writer.writeBeatGrid(id, g3, own).outcome == Outcome::RowEditedOnEngine,
              "a grid moved on the player is refused, even with the last write's fingerprint");
        own.ownGridFingerprint = std::string(64, '0');
        check(writer.writeBeatGrid(id, g3, own).outcome == Outcome::RowEditedOnEngine, "and with a wrong one");
        check(allBeatData(copy.mdb) == movedBytes, "neither wrote anything");

        BeatGridWriteOptions overwrite;
        overwrite.overwriteEngineEdits = true;
        const auto third = writer.writeBeatGrid(id, g3, overwrite);
        check(third.outcome == Outcome::Written, "overwriting writes G3");
        exec(copy.mdb, "UPDATE Track SET isBeatGridLocked = 1 WHERE id = " + id);
        own.ownGridFingerprint = third.gridFingerprint;
        const auto lockedBytes = allBeatData(copy.mdb);
        check(writer.writeBeatGrid(id, g4, own).outcome == Outcome::RowLocked, "a lock refuses even our own grid");
        check(allBeatData(copy.mdb) == lockedBytes, "and nothing is written");
        std::cout << "case 9 (own earlier write) done\n";
    }

    // 10. The hook before a write: called once per write and never for a
    // refusal or a grid already there; a hook that throws stops the write.
    {
        const Copy copy = freshCopy("before-write");
        const BeatData denon = *beatDataOf(copy.mdb, kConstant);
        const double durationMs = denon.samples / denon.sample_rate * 1000.0;
        const auto grid = constantGrid(37.0, 60000.0 / 121.0, durationMs, 1);
        const std::string id = std::to_string(kConstant);
        auto writer = unshiftedWriter(copy);
        int calls = 0;
        bool fail = true;
        writer.setBeforeBeatGridWrite([&] {
            ++calls;
            if (fail) {
                throw std::runtime_error("no backup");
            }
        });
        const auto before = allBeatData(copy.mdb);
        bool threw = false;
        try {
            writer.writeBeatGrid(id, grid);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        check(threw && calls == 1, "a hook that throws leaves writeBeatGrid");
        check(allBeatData(copy.mdb) == before, "and nothing is written");
        fail = false;
        check(writer.writeBeatGrid(id, grid).outcome == Outcome::Written && calls == 2, "the hook runs before a write");
        check(writer.writeBeatGrid(id, grid).outcome == Outcome::AlreadySame && calls == 2, "not for a grid already there");
        check(writer.writeBeatGrid(id, constantGrid(41.0, 500.0, durationMs, 1)).outcome == Outcome::RowEditedOnEngine
                  && calls == 2,
              "nor for a refusal");
        std::cout << "case 10 (the hook before a write) done\n";
    }

    // 11. A preview: every row answers as the real call answers it, the
    // fingerprint of the grid it would leave included, and not a byte of
    // the database changes, nor is the hook before a write called. Over
    // an analysed row, a locked one, one edited on the player, one the
    // player has not analysed (Skip, Fill, FillAndLock), and a row whose
    // grid is already the one given.
    {
        const Copy copy = freshCopy("preview");
        exec(copy.mdb, "UPDATE Track SET isBeatGridLocked = 1 WHERE id = " + std::to_string(kLocked));
        BeatData edited = *beatDataOf(copy.mdb, kEdited);
        for (auto &marker : edited.adjusted_beat_grid) {
            marker.sample_offset += 441.0;
        }
        {
            auto db = djinterop::engine::v3::engine_library::load(pathToUtf8(copy.root));
            db.performance_data().set_beat_data(kEdited, edited);
        }
        const auto lengthOf = [&](std::int64_t id) {
            const BeatData blob = *beatDataOf(copy.mdb, id);
            return blob.samples / blob.sample_rate * 1000.0;
        };
        struct Case
        {
            std::string what;
            std::int64_t id;
            std::vector<domain::Beat> beats;
            BeatGridWriteOptions options;
        };
        BeatGridWriteOptions fill;
        fill.unanalysed = UnanalysedRowGrid::Fill;
        fill.fileSampleRateHz = 48000.0;
        fill.fileDurationMs = 200000.0;
        BeatGridWriteOptions fillAndLock = fill;
        fillAndLock.unanalysed = UnanalysedRowGrid::FillAndLock;
        const std::vector<Case> cases = {
            {"an analysed row", kConstant, constantGrid(37.0, 60000.0 / 121.0, lengthOf(kConstant), 1), {}},
            {"a locked row", kLocked, constantGrid(50.0, 451.0, lengthOf(kLocked), 1), {}},
            {"a row edited on the player", kEdited, constantGrid(50.0, 488.0, lengthOf(kEdited), 1), {}},
            {"an unanalysed row, Skip", kUnanalysed, constantGrid(120.0, 480.0, 200000.0, 1), {}},
            {"an unanalysed row, Fill", kUnanalysed, constantGrid(120.0, 480.0, 200000.0, 1), fill},
            {"an unanalysed row, FillAndLock", kUnanalysedToo, constantGrid(120.0, 480.0, 200000.0, 1), fillAndLock},
        };

        const auto fileBytes = [&]() {
            std::ifstream in(pathFromUtf8(copy.mdb), std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        };
        const std::string bytesBefore = fileBytes();
        const auto beatDataBefore = allBeatData(copy.mdb);
        const std::string lockBefore = trackColumn(copy.mdb, kUnanalysedToo, "isBeatGridLocked");
        std::vector<BeatGridWriteResult> previews;
        int hookCalls = 0;
        {
            auto preview = unshiftedWriter(copy);
            preview.setBeforeBeatGridWrite([&] { ++hookCalls; });
            for (const Case &c : cases) {
                BeatGridWriteOptions options = c.options;
                options.previewOnly = true;
                previews.push_back(preview.writeBeatGrid(std::to_string(c.id), c.beats, options));
            }
        }
        check(hookCalls == 0, "a preview never calls the hook before a write, " + std::to_string(hookCalls) + " calls");
        check(fileBytes() == bytesBefore, "a preview leaves m.db byte for byte as it was");
        check(allBeatData(copy.mdb) == beatDataBefore, "and every row's beat data");
        check(trackColumn(copy.mdb, kUnanalysedToo, "isBeatGridLocked") == lockBefore, "and locks nothing");

        auto real = unshiftedWriter(copy);
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const auto done = real.writeBeatGrid(std::to_string(cases[i].id), cases[i].beats, cases[i].options);
            check(previews[i].outcome == done.outcome, cases[i].what + ": the preview's outcome is the real call's");
            check(previews[i].gridFingerprint == done.gridFingerprint,
                  cases[i].what + ": and so is the fingerprint of the grid it leaves");
            check(previews[i].markers == done.markers && previews[i].barBreaks == done.barBreaks,
                  cases[i].what + ": and its markers and bar breaks");
        }
        check(previews[0].outcome == Outcome::Written && previews[1].outcome == Outcome::RowLocked
                  && previews[2].outcome == Outcome::RowEditedOnEngine && previews[3].outcome == Outcome::RowNotAnalysed
                  && previews[4].outcome == Outcome::Written && previews[5].outcome == Outcome::Written,
              "the cases are the outcomes they are named for");

        // The analysed row's grid is there now: the same grid again.
        const std::string bytesAfter = fileBytes();
        BeatGridWriteOptions preview;
        preview.previewOnly = true;
        const auto same = real.writeBeatGrid(std::to_string(kConstant), cases[0].beats, preview);
        check(same.outcome == Outcome::AlreadySame, "a grid already there previews as AlreadySame");
        check(same.gridFingerprint == real.writeBeatGrid(std::to_string(kConstant), cases[0].beats).gridFingerprint,
              "with the real call's fingerprint");
        check(fileBytes() == bytesAfter, "and neither wrote anything");
        std::cout << "case 11 (preview) done\n";
    }

    if (failures == 0) {
        std::cout << "libdjinterop_engine_beat_grid_writer_test: all cases passed\n";
    }
    return failures == 0 ? 0 : 1;
}
