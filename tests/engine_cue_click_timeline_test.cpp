// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// #123: a cue added by clicking an Engine track's waveform in the details
// pane is written where the click was. The pane draws an Engine MP3 on
// Engine's timeline (cuesOnGridTimeline(), cueToGridShiftMs()); the cue
// writer takes rekordbox's. The pane takes cueToGridShiftMs() off the
// click (PlaybackController::positionOnCueTimeline()) and stages that.
//
// Through a real scratch Engine library and the beat-click MP3s, for a
// click at X ms on the pane:
//
//   - the cue the writer stores sits X ms into the file on Engine's
//     timeline, within one sample;
//   - read back and drawn on the pane again, it is at X, within one
//     sample;
//   - for a LAME file the click moves by 51.18 ms, for an FFmpeg (Lavc)
//     one by about 25 ms, for a WAV not at all.
//
// Usage: engine_cue_click_timeline_test <beat-click dir> <mp3-lead-in dir>

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>

#include <djinterop/djinterop.hpp>

#include "application/use_cases/scan_library.hpp"
#include "domain/matching_policy.hpp"
#include "infrastructure/audio/playback_timeline.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_library_creator.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using namespace seabass::infrastructure::engine;
using seabass::domain::CuePoint;
using seabass::domain::MatchingPolicy;
using seabass::domain::Track;
using seabass::infrastructure::audio::cueToGridShiftMs;
using seabass::infrastructure::audio::GridTimeline;
namespace fs = std::filesystem;

namespace
{

constexpr double ClickMs = 5000.0;
constexpr double LoopEndClickMs = 7000.0;
constexpr double Rate = 44100.0;
constexpr double OneSampleMs = 1000.0 / Rate;

struct Library
{
    fs::path root;
    std::string file;
};

Library makeLibrary(const std::string &caseName, const fs::path &source)
{
    Library lib;
    lib.root = seabass::testing::scratchRoot() / "seabass_engine_cue_click_timeline_test"
        / seabass::pathFromUtf8(caseName) / "Engine Library";
    fs::remove_all(lib.root.parent_path());
    fs::create_directories(lib.root);
    const fs::path file = lib.root / seabass::pathFromUtf8("song" + seabass::pathToUtf8(source.extension()));
    fs::copy_file(source, file);
    lib.file = seabass::pathToUtf8(file);

    Track t;
    t.sourceId = "r0";
    t.title = "Click";
    t.artist = "Artist";
    t.filePath = lib.file;
    t.bpm = 120.0;
    t.durationSeconds = 10.0;
    const auto created = EngineLibraryCreator::create(seabass::pathToUtf8(lib.root), {t}, EngineSchemaGeneration::V2);
    assert(created.errorMessage.empty() && created.tracksCreated == 1);
    (void)created;
    return lib;
}

Track readOne(const Library &lib)
{
    LibdjinteropEngineReader reader(seabass::pathToUtf8(lib.root));
    auto tracks = seabass::application::ScanLibrary(reader).execute();
    assert(tracks.size() == 1);
    return tracks.front();
}

const CuePoint *findHot(const std::vector<CuePoint> &cues, int pad, bool loop)
{
    for (const auto &c : cues) {
        if (c.kind == CuePoint::Kind::Hot && c.hotCueNumber == pad && c.isLoop == loop) {
            return &c;
        }
    }
    return nullptr;
}

// A hot cue and a hot loop added by clicks on the pane at ClickMs (and a
// drag to LoopEndClickMs), each staged as the pane stages it; returns the
// shift the pane took off. Asserts the stored samples and what the pane
// draws after reading the library again.
double clickAndCheck(const std::string &caseName, const fs::path &fixture)
{
    const Library lib = makeLibrary(caseName, fixture);
    const Track before = readOne(lib);

    // What the pane draws an Engine row's cues by, and what it now takes
    // off a click before staging it.
    const double shift = cueToGridShiftMs(lib.file, GridTimeline::Engine);
    CuePoint hot{CuePoint::Kind::Hot, 1, ClickMs - shift, "#ffcc00", ""};
    CuePoint loop{CuePoint::Kind::Hot, 2, ClickMs - shift, "#3daee9", ""};
    loop.isLoop = true;
    loop.loopEndMs = LoopEndClickMs - shift;

    LibdjinteropEngineCueWriter writer(seabass::pathToUtf8(lib.root));
    writer.writeHotCues(before.sourceId, {hot, loop});

    // Engine holds the cue X ms into the file on its own timeline.
    auto db = djinterop::engine::load_database(seabass::pathToUtf8(lib.root));
    auto row = db.track_by_id(std::stoll(before.sourceId));
    assert(row);
    const auto hotCues = row->hot_cues();
    assert(hotCues.size() > 0 && hotCues[0]);
    assert(std::abs(hotCues[0]->sample_offset - ClickMs * Rate / 1000.0) <= 1.0);
    const auto loops = row->loops();
    assert(loops.size() > 1 && loops[1]);
    assert(std::abs(loops[1]->start_sample_offset - ClickMs * Rate / 1000.0) <= 1.0);
    assert(std::abs(loops[1]->end_sample_offset - LoopEndClickMs * Rate / 1000.0) <= 1.0);

    // Read back and drawn on the pane: where the click was.
    const Track after = readOne(lib);
    const CuePoint *readHot = findHot(after.cues, 1, false);
    const CuePoint *readLoop = findHot(after.cues, 2, true);
    assert(readHot && readLoop);
    const double drawShift = cueToGridShiftMs(lib.file, GridTimeline::Engine);
    assert(std::abs(readHot->positionMs + drawShift - ClickMs) <= OneSampleMs);
    assert(std::abs(readLoop->positionMs + drawShift - ClickMs) <= OneSampleMs);
    assert(std::abs(readLoop->loopEndMs + drawShift - LoopEndClickMs) <= OneSampleMs);
    std::cout << caseName << ": click at " << ClickMs << " ms staged at " << hot.positionMs << " ms, stored at "
              << hotCues[0]->sample_offset << " samples, drawn at " << readHot->positionMs + drawShift << " ms\n";
    return shift;
}

}  // namespace

int main(int argc, char **argv)
{
    assert(argc == 3);
    const fs::path beatClick = seabass::pathFromUtf8(argv[1]);
    const fs::path leadIn = seabass::pathFromUtf8(argv[2]);
    MatchingPolicy::reset();
    assert(MatchingPolicy::correctMp3CueTiming());

    // LAME: the encoder delay, FFmpeg's 529 and the Info frame, 2257
    // samples at 44.1 kHz.
    const double lame = clickAndCheck("lame", beatClick / "beats-D-lame-44100.mp3");
    assert(std::abs(lame + 2257.0 * 1000.0 / Rate) < 1e-6);
    (void)lame;

    // FFmpeg's own encoder: about 25 ms.
    const double lavc = clickAndCheck("lavc", beatClick / "beats-B-lavc-44100.mp3");
    assert(lavc < -24.0 && lavc > -27.0);
    (void)lavc;

    // A WAV is drawn and written on one timeline: the click as it is.
    const double wav = clickAndCheck("wav", leadIn / "silence.wav");
    assert(wav == 0.0);
    (void)wav;

    std::cout << "a click on the pane is written where it was\n";
    return 0;
}
