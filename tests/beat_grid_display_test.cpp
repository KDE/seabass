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
#include <iostream>
#include <string>
#include <vector>

#include <QVariantMap>

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

    // ---- Engine: the rate the row records, or no grid -----------------
    {
        const fs::path root = testing::scratchRoot() / ("seabass-beat-grid-display-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "Contents", ec);
        const fs::path libraryPath = infrastructure::engine::engineLibraryPath(root);

        std::vector<domain::Track> tracks;
        for (const char *id : {"1", "2"}) {
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
        check(created.errorMessage.empty() && created.tracksCreated == 2, "the scratch library is created");

        std::string recordedId;
        std::string unrecordedId;
        {
            auto db = djinterop::engine::load_database(pathToUtf8(libraryPath));
            auto rows = db.tracks();
            check(rows.size() == 2, "two rows");
            if (rows.size() == 2) {
                // One analysed at 48 kHz, its rate recorded.
                auto analysed = rows[0].snapshot();
                analysed.bpm = 120.0;
                analysed.sample_rate = 48000.0;
                analysed.sample_count = 48000ULL * 60;
                analysed.duration = std::chrono::milliseconds(60000);
                analysed.beatgrid = markers(24000.0);
                rows[0].update(analysed);
                recordedId = std::to_string(rows[0].id());

                // One whose markers are there but whose rate is not: the
                // reader works out 44.1 kHz from them and the tempo.
                auto unrated = rows[1].snapshot();
                unrated.bpm = 120.0;
                unrated.sample_rate = std::nullopt;
                unrated.duration = std::chrono::milliseconds(60000);
                unrated.beatgrid = markers(22050.0);
                rows[1].update(unrated);
                unrecordedId = std::to_string(rows[1].id());
                check(!rows[1].sample_rate().has_value(), "the second row records no rate");
                check(rows[1].beatgrid().size() == 2, "but does have its markers");
            }
        }

        const auto recorded = infrastructure::engine::readTrackAnalysis(pathToUtf8(libraryPath), recordedId);
        check(recorded.beats.size() >= 119, "the analysed row has its grid, got " + std::to_string(recorded.beats.size()));
        check(recorded.beatRateRecorded, "at the rate it records");
        const QVariantList drawn = gui::beatGridForDisplay(recorded.beats, recorded.beatRateRecorded);
        check(drawn.size() == static_cast<qsizetype>(recorded.beats.size()), "and all of it is drawn");
        if (drawn.size() > 2) {
            check(std::abs(drawn[2].toMap().value("timeMs").toDouble() - 1000.0) < 0.01,
                  "beat 2 at 1 s at 48 kHz, got " + std::to_string(drawn[2].toMap().value("timeMs").toDouble()));
            check(drawn[0].toMap().value("downbeat").toBool() && !drawn[1].toMap().value("downbeat").toBool(),
                  "beat 0 starts a bar, beat 1 does not");
        }

        const auto unrecorded = infrastructure::engine::readTrackAnalysis(pathToUtf8(libraryPath), unrecordedId);
        check(!unrecorded.beats.empty(), "the reader still gives the player a grid to keep time by");
        check(!unrecorded.beatRateRecorded, "and says its rate is not the library's");
        check(gui::beatGridForDisplay(unrecorded.beats, unrecorded.beatRateRecorded).isEmpty(),
              "so no grid is drawn for it");

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
