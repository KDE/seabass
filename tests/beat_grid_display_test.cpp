// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The beat grid a waveform draws (#40), from the read the waveform comes
// from: every beat with its time and whether it starts a bar, and none at
// all for an Engine row that records no sample rate. Engine stores beats
// as sample offsets; without the rate they count in, the times are worked
// out, and a grid drawn from a working-out is a grid presented as fact
// that may not be one. The player still keeps time by it; the display
// shows nothing.

#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <iostream>
#include <string>
#include <vector>

#include <QVariantMap>

#include <sqlite3.h>

#include <djinterop/djinterop.hpp>

#include "domain/track.hpp"
#include "gui/beat_grid_display.hpp"
#include "infrastructure/engine/engine_library_layout.hpp"
#include "infrastructure/engine/libdjinterop_engine_library_creator.hpp"
#include "infrastructure/engine/libdjinterop_waveform_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_waveform_reader.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;

namespace
{

int failures = 0;

// What a waveform is handed for a track's analysis.
QVariantList drawnGrid(const domain::TrackAnalysis &analysis)
{
    return gui::beatGridForDisplay(analysis.beats, analysis.beatRateRecorded);
}

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

// 120 BPM for a minute: markers a beat before the start and past the end,
// the way Engine stores them, `samplesPerBeat` apart.
std::vector<djinterop::beatgrid_marker> markers(double samplesPerBeat)
{
    return {{-4, -4 * samplesPerBeat}, {124, 124 * samplesPerBeat}};
}

}  // namespace

int main()
{
    // ---- The conversion itself --------------------------------------
    {
        const std::vector<domain::Beat> beats = {{100.0, 1}, {600.0, 2}, {1100.0, 3}, {1600.0, 4},
                                                 {2100.0, 1}, {2600.0, 0}};
        const QVariantList grid = gui::beatGridForDisplay(beats, true, 0.0);
        check(grid.size() == 6, "every beat is handed over");
        const bool expected[] = {true, false, false, false, true, false};
        for (qsizetype i = 0; i < grid.size(); ++i) {
            const QVariantMap entry = grid[i].toMap();
            check(entry.value("timeMs").toDouble() == beats[static_cast<size_t>(i)].timeMs, "time of beat " + std::to_string(i));
            check(entry.value("downbeat").toBool() == expected[i], "downbeat of beat " + std::to_string(i));
        }
        const QVariantList shifted = gui::beatGridForDisplay(beats, true, -51.0);
        check(shifted.size() == 6 && shifted[0].toMap().value("timeMs").toDouble() == 49.0,
              "the player's timeline shift moves every beat");
        check(gui::beatGridForDisplay(beats, false, 0.0).isEmpty(), "a grid at an unrecorded rate is not drawn");
        check(gui::beatGridForDisplay({}, true, 0.0).isEmpty(), "no grid, no ticks");
    }

    // ---- The density rule, in C++ ------------------------------------
    // tst_WaveformBeatGrid tests it case by case through BeatGridTicks;
    // here, what a list row relies on: the beats it is handed for a width
    // lay out at that width as exactly the ticks the whole grid does.
    {
        std::vector<domain::Beat> track;   // 10 minutes at 128 BPM
        for (int i = 0; i < 1280; ++i) {
            track.push_back({i * 60000.0 / 128.0, i % 4 + 1});
        }
        // and one with a tempo change: 120 BPM for a minute, then 60.
        std::vector<domain::Beat> change;
        for (int i = 0; i < 120; ++i) {
            change.push_back({i * 500.0, i % 4 + 1});
        }
        for (int i = 0; i < 60; ++i) {
            change.push_back({60000.0 + i * 1000.0, i % 4 + 1});
        }
        struct Case { const std::vector<domain::Beat> *beats; double spanMs; };
        for (const Case &c : {Case{&track, 600000.0}, Case{&change, 120000.0}}) {
            for (double width : {150.0, 300.0, 600.0, 960.0, 1200.0, 1920.0, 5000.0}) {
                const gui::BeatTickLayout whole = gui::layoutBeatTicks(*c.beats, c.spanMs, width);
                const QVariantList kept = gui::beatTicksForDisplay(*c.beats, true, c.spanMs, width);
                check(kept.size() == static_cast<qsizetype>(whole.ticks.size()), "a row keeps the beats drawn");
                std::vector<domain::Beat> again;
                for (const QVariant &entry : kept) {
                    const QVariantMap beat = entry.toMap();
                    again.push_back({beat.value("timeMs").toDouble(), beat.value("downbeat").toBool() ? 1 : 0});
                }
                const gui::BeatTickLayout relaid = gui::layoutBeatTicks(again, c.spanMs, width);
                bool same = relaid.ticks.size() == whole.ticks.size();
                for (size_t i = 0; same && i < whole.ticks.size(); ++i) {
                    same = relaid.ticks[i].x == whole.ticks[i].x && relaid.ticks[i].downbeat == whole.ticks[i].downbeat;
                }
                check(same, "laid out again at " + std::to_string(width) + " px they are the same ticks");
                for (size_t i = 1; i < whole.ticks.size(); ++i) {
                    const double gap = whole.ticks[i].x - whole.ticks[i - 1].x;
                    check(gap >= (whole.everyBeat ? gui::MinimumBeatSpacingPx : gui::MinimumDownbeatSpacingPx) - 1e-9,
                          "no two ticks closer than the rule allows at " + std::to_string(width) + " px");
                }
            }
        }
        // One bad pair must not thin the whole track: a beat duplicated a
        // millisecond after itself (a damaged grid), a downbeat among them,
        // lays out exactly as the grid without it, at every width.
        {
            const auto sameLayout = [](const gui::BeatTickLayout &a, const gui::BeatTickLayout &b) {
                if (a.ticks.size() != b.ticks.size() || a.everyBeat != b.everyBeat || a.barsPerTick != b.barsPerTick) {
                    return false;
                }
                for (size_t i = 0; i < a.ticks.size(); ++i) {
                    if (a.ticks[i].timeMs != b.ticks[i].timeMs || a.ticks[i].downbeat != b.ticks[i].downbeat) {
                        return false;
                    }
                }
                return true;
            };
            std::vector<domain::Beat> damaged = track;
            damaged.insert(damaged.begin() + 501, {track[500].timeMs + 1.0, track[500].beatInBar});   // a downbeat
            damaged.insert(damaged.begin() + 900, {track[898].timeMs + 1.0, track[898].beatInBar});   // an ordinary beat
            for (double width : {300.0, 600.0, 1200.0, 2400.0, 5000.0}) {
                const auto clean = gui::layoutBeatTicks(track, 600000.0, width);
                const auto withPair = gui::layoutBeatTicks(damaged, 600000.0, width);
                check(sameLayout(clean, withPair), "a duplicated beat changes nothing at " + std::to_string(width)
                      + " px: " + std::to_string(withPair.ticks.size()) + " ticks, every " + std::to_string(withPair.barsPerTick)
                      + " bars, against " + std::to_string(clean.ticks.size()) + ", every " + std::to_string(clean.barsPerTick));
            }
            // Out of order: laid out as in order, not as nonsense.
            std::vector<domain::Beat> shuffled = track;
            std::reverse(shuffled.begin() + 100, shuffled.begin() + 700);
            std::swap(shuffled[3], shuffled[1000]);
            for (double width : {600.0, 5000.0}) {
                check(sameLayout(gui::layoutBeatTicks(track, 600000.0, width), gui::layoutBeatTicks(shuffled, 600000.0, width)),
                      "unsorted beats lay out as sorted ones at " + std::to_string(width) + " px");
            }
            // A real tempo change, double time, still thins by its tight half.
            std::vector<domain::Beat> doubled;
            for (int i = 0; i < 120; ++i) {
                doubled.push_back({i * 500.0, i % 4 + 1});
            }
            for (int i = 0; i < 240; ++i) {
                doubled.push_back({60000.0 + i * 250.0, i % 4 + 1});
            }
            const auto atDouble = gui::layoutBeatTicks(doubled, 120000.0, 3840.0);   // 16 px, then 8 px a beat
            check(atDouble.everyBeat && atDouble.ticks.size() == 360, "8 px at double time: every beat, none dropped");
            const auto narrow = gui::layoutBeatTicks(doubled, 120000.0, 1920.0);   // 4 px at double time
            check(!narrow.everyBeat && narrow.barsPerTick == 1 && narrow.ticks.size() == 90,
                  "4 px at double time: the downbeats, all 90 of them, got " + std::to_string(narrow.ticks.size()));
        }

        const gui::BeatTickLayout whole = gui::layoutBeatTicks(track, 600000.0, 600.0);
        check(!whole.everyBeat && whole.barsPerTick == 8 && whole.ticks.size() == 40,
              "a whole track at 600 px: every 8th downbeat, 40 ticks");
        check(gui::beatTicksForDisplay(track, false, 600000.0, 600.0).isEmpty(), "an unrecorded rate keeps none");
        check(gui::layoutBeatTicks(track, 0.0, 600.0).ticks.empty(), "no length, no ticks");
        check(gui::layoutBeatTicks(track, 600000.0, 0.0).ticks.empty(), "no width, no ticks");
    }

    // ---- Engine: the rate the row records, or no grid -----------------
    {
        const fs::path root = testing::scratchRoot() / ("seabass-beat-grid-display-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "Contents", ec);
        const fs::path libraryPath = infrastructure::engine::engineLibraryPath(root);

        std::vector<domain::Track> tracks;
        for (const char *id : {"1", "2", "3", "4", "5"}) {
            domain::Track track;
            track.sourceId = id;
            track.title = std::string("Track ") + id;
            track.artist = "An Artist";
            track.filePath = pathToUtf8(root / "Contents" / (std::string(id) + ".mp3"));
            track.bpm = 120.0;
            track.durationSeconds = 60.0;
            tracks.push_back(track);
        }
        const auto created = infrastructure::engine::EngineLibraryCreator::create(
            pathToUtf8(libraryPath), tracks, infrastructure::engine::EngineSchemaGeneration::V2);
        check(created.errorMessage.empty() && created.tracksCreated == 5, "the scratch library is created");

        // Five rows, as Engine libraries have them:
        //   0: analysed at 48 kHz, the rate recorded;
        //   1: markers, no rate recorded: 22050 samples a beat at 120 BPM
        //      is 44.1 kHz, which the beat reader works out from them;
        //   2: the same, its data record damaged so that libdjinterop
        //      throws reading the rate;
        //   3: markers, no rate, and a tempo they do not fit at all;
        //   4: markers, no rate, and the tempo stored at double the grid's
        //      (240 against 120, as drum and bass is stored at 174 against
        //      a grid at 87): the reader takes the double rate, 88.2 kHz,
        //      and would put the grid in the first half of the track.
        // Only a rate the row itself gives is drawn; a worked-out one is
        // not, however likely it looks. The player keeps time by what the
        // reader gives either way.
        std::vector<std::string> ids;
        {
            auto db = djinterop::engine::load_database(pathToUtf8(libraryPath));
            auto rows = db.tracks();
            check(rows.size() == 5, "five rows");
            const struct { std::optional<double> rate; double samplesPerBeat; double bpm; } shapes[] = {
                {48000.0, 24000.0, 120.0}, {std::nullopt, 22050.0, 120.0}, {std::nullopt, 22050.0, 120.0},
                {std::nullopt, 22050.0, 100.0}, {std::nullopt, 22050.0, 240.0}};
            for (size_t i = 0; i < rows.size() && i < 5; ++i) {
                auto snapshot = rows[i].snapshot();
                snapshot.bpm = shapes[i].bpm;
                snapshot.sample_rate = shapes[i].rate;
                if (shapes[i].rate) {
                    snapshot.sample_count = static_cast<unsigned long long>(*shapes[i].rate * 60);
                }
                snapshot.duration = std::chrono::milliseconds(60000);
                snapshot.beatgrid = markers(shapes[i].samplesPerBeat);
                rows[i].update(snapshot);
                ids.push_back(std::to_string(rows[i].id()));
            }
        }
        check(ids.size() == 5, "five ids");
        if (ids.size() == 5) {
            // Damage row 2's data record, wherever this schema keeps it.
            sqlite3 *handle = nullptr;
            check(sqlite3_open(pathToUtf8(libraryPath / "Database2" / "m.db").c_str(), &handle) == SQLITE_OK,
                  "the scratch database opens");
            const std::string damage = "UPDATE PerformanceData SET trackData = X'00' WHERE trackId = " + ids[2];
            const std::string damageTrack = "UPDATE Track SET trackData = X'00' WHERE id = " + ids[2];
            const bool damaged = sqlite3_exec(handle, damage.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK
                || sqlite3_exec(handle, damageTrack.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
            sqlite3_close(handle);
            check(damaged, "row 2's record is damaged");
            bool throws = false;
            try {
                auto db = djinterop::engine::load_database(pathToUtf8(libraryPath));
                (void)db.track_by_id(std::stoll(ids[2]))->sample_rate();
            } catch (const std::exception &) {
                throws = true;
            }
            check(throws, "libdjinterop throws reading row 2's rate");

            const auto beatAt = [](const QVariantList &grid, int i) { return grid[i].toMap().value("timeMs").toDouble(); };
            const auto recorded = infrastructure::engine::readTrackAnalysis(pathToUtf8(libraryPath), ids[0]);
            check(recorded.beatRateRecorded, "row 0: the rate is the row's");
            const QVariantList drawn = drawnGrid(recorded);
            check(drawn.size() >= 119 && drawn.size() == static_cast<qsizetype>(recorded.beats.size()),
                  "row 0: the whole grid is drawn, got " + std::to_string(drawn.size()));
            if (drawn.size() > 4) {
                check(std::abs(beatAt(drawn, 2) - 1000.0) < 0.01, "row 0: beat 2 at 1 s at 48 kHz");
                check(std::abs(beatAt(drawn, 4) - 2000.0) < 0.01, "row 0: beat 4 at 2 s");
                check(drawn[0].toMap().value("downbeat").toBool() && !drawn[1].toMap().value("downbeat").toBool(),
                      "beat 0 starts a bar, beat 1 does not");
            }
            const char *withheld[] = {nullptr, "row 1, a rate worked out from the markers",
                                      "row 2, its rate unreadable and worked out", "row 3, no rate that fits",
                                      "row 4, the tempo stored at double the grid's"};
            for (int i = 1; i < 5; ++i) {
                const auto analysis = infrastructure::engine::readTrackAnalysis(pathToUtf8(libraryPath), ids[i]);
                check(!analysis.beatRateRecorded, std::string(withheld[i]) + ": not the row's rate");
                check(drawnGrid(analysis).isEmpty(), std::string(withheld[i]) + ": no grid drawn");
                check(gui::beatTicksForDisplay(analysis.beats, analysis.beatRateRecorded, 60000.0, 1200.0).isEmpty(),
                      std::string(withheld[i]) + ": no ticks for a row either");
            }
            // What the player keeps time by is the reader's, as before.
            check(!infrastructure::engine::readTrackAnalysis(pathToUtf8(libraryPath), ids[1]).beats.empty(),
                  "row 1 still gives the player its beats");
            const auto halved = infrastructure::engine::readTrackAnalysis(pathToUtf8(libraryPath), ids[4]);
            check(!halved.beats.empty() && halved.beats.back().timeMs <= 31000.5,
                  "row 4: the reader's beats are squeezed into the first half, which is why none is drawn");
        }

        // The committed fixture, on a copy (opening a fixture database in
        // place can rewrite or delete its files): every analysed track's
        // rate is read off its row now, so every grid it has is drawn.
        {
            const fs::path copy = root / "fixture-engine";
            fs::copy(pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library" / "engine", copy,
                     fs::copy_options::recursive, ec);
            check(!ec, "the fixture's Engine library is copied");
            const std::string enginePath = pathToUtf8(copy);
            auto db = djinterop::engine::load_database(enginePath);
            int analysed = 0;
            int withBeats = 0;
            int rateRecorded = 0;
            int drawn = 0;
            for (const auto &track : db.tracks()) {
                if (track.beatgrid().size() < 2) {
                    continue;
                }
                ++analysed;
                const auto analysis = infrastructure::engine::readTrackAnalysis(enginePath, std::to_string(track.id()));
                withBeats += analysis.beats.empty() ? 0 : 1;
                rateRecorded += analysis.beatRateRecorded ? 1 : 0;
                drawn += drawnGrid(analysis).isEmpty() ? 0 : 1;
            }
            std::cout << "fixture Engine library: " << analysed << " analysed tracks, " << withBeats << " with beats, "
                      << rateRecorded << " with the rate on the row, " << drawn << " with a grid drawn\n";
            check(analysed > 300 && drawn == withBeats,
                  "every analysed fixture track with beats has its rate on the row and its grid drawn, got "
                      + std::to_string(drawn) + " of " + std::to_string(withBeats));
        }

        fs::remove_all(root, ec);
    }

    // ---- rekordbox: times, no rate to doubt --------------------------
    {
        const std::string pioneer = std::string(SEABASS_SOURCE_DIR) + "/tests/fixtures/anonymized_library/rekordbox";
        infrastructure::rekordbox::KaitaiRekordboxReader reader(pioneer);
        int withGrid = 0;
        int looked = 0;
        for (const auto &track : reader.readAll()) {
            if (looked++ >= 20) {
                break;
            }
            const auto analysis = infrastructure::rekordbox::readTrackAnalysis(pioneer, track.sourceId);
            if (analysis.beats.empty()) {
                continue;
            }
            ++withGrid;
            check(analysis.beatRateRecorded, "a rekordbox grid is in milliseconds already");
            const QVariantList drawn = gui::beatGridForDisplay(analysis.beats, analysis.beatRateRecorded);
            check(drawn.size() == static_cast<qsizetype>(analysis.beats.size()), "every rekordbox beat is drawn");
        }
        check(withGrid > 0, "the fixture has rekordbox tracks with a grid");
    }

    if (failures == 0) {
        std::cout << "beat_grid_display_test: all cases passed\n";
    }
    return failures == 0 ? 0 : 1;
}
