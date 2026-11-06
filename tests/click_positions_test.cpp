// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// click_positions (#50, layer 3) on a scratch stick built from the
// committed one_stick_two_catalogs fixture: one MP3 both catalogs hold
// gets the class D click file as its audio, Engine hot cues planted raw
// (no lead-in correction) and rekordbox hot cues on the same pads. The
// tool must print, per pad, corrected Engine minus rekordbox: the planted
// Engine minus rekordbox difference plus the predicted offset; the same
// for the first beat of a planted Engine beat grid. And the
// stick must be byte for byte as it was: the tool only reads.
//
// Usage: click_positions_test <one_stick_two_catalogs dir> <click mp3> <click_positions binary>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <djinterop/djinterop.hpp>

#include "domain/mp3_lead_in.hpp"
#include "domain/track.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_beat_grid_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
using domain::CuePoint;
using domain::Track;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
    if (!ok) {
        ++failures;
    }
}

CuePoint hot(int number, double ms)
{
    CuePoint cue;
    cue.kind = CuePoint::Kind::Hot;
    cue.hotCueNumber = number;
    cue.positionMs = ms;
    return cue;
}

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << in.rdbuf();
    return bytes.str();
}

// Every file under the stick and its bytes.
std::map<std::string, std::string> snapshot(const fs::path &root)
{
    std::map<std::string, std::string> files;
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            files[pathToUtf8(entry.path())] = readFile(entry.path());
        }
    }
    return files;
}

std::string runTool(const std::string &tool, const fs::path &stick)
{
    const std::string command = "'" + tool + "' '" + pathToUtf8(stick) + "' 2>/dev/null";
    std::string output;
    FILE *pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return output;
    }
    char buffer[4096];
    while (std::fgets(buffer, sizeof buffer, pipe)) {
        output += buffer;
    }
    const int status = pclose(pipe);
    check(status == 0, "click_positions exits 0");
    return output;
}

// The lines of the tool's block for one file: from its path line to the
// next line that does not start with two spaces.
std::vector<std::string> blockFor(const std::string &output, const std::string &relativePath)
{
    std::vector<std::string> block;
    std::istringstream lines(output);
    std::string line;
    bool inside = false;
    while (std::getline(lines, line)) {
        if (inside && line.rfind("  ", 0) != 0) {
            break;
        }
        if (inside) {
            block.push_back(line);
        }
        if (line.rfind(relativePath + "  ", 0) == 0) {
            inside = true;
        }
    }
    return block;
}

std::optional<double> residualOn(const std::vector<std::string> &block, const std::string &pad)
{
    for (const std::string &line : block) {
        if (line.rfind("  pad " + pad + ":", 0) == 0) {
            const auto at = line.find("residual ");
            if (at != std::string::npos) {
                return std::stod(line.substr(at + 9));
            }
        }
    }
    return std::nullopt;
}

std::string keyOf(const std::string &filePath)
{
    return pathToUtf8(pathFromUtf8(filePath).lexically_normal());
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::cerr << "usage: click_positions_test <one_stick_two_catalogs dir> <click mp3> <click_positions>\n";
        return 2;
    }
    const fs::path fixture = pathFromUtf8(argv[1]);
    const fs::path clickFile = pathFromUtf8(argv[2]);
    const std::string tool = argv[3];

    const fs::path stick = testing::scratchRoot() / "click_positions_test";
    fs::remove_all(stick);
    fs::create_directories(stick);
    fs::copy(fixture / "rekordbox", stick / "PIONEER", fs::copy_options::recursive);
    fs::copy(fixture / "engine", stick / "Engine Library", fs::copy_options::recursive);
    const std::string pioneer = pathToUtf8(stick / "PIONEER");
    const std::string engineLibrary = pathToUtf8(stick / "Engine Library");

    // The first MP3 both catalogs hold.
    infrastructure::rekordbox::KaitaiRekordboxReader rekordboxReader(pioneer);
    const std::vector<Track> rekordboxTracks = rekordboxReader.readTracks();
    infrastructure::engine::LibdjinteropEngineReader engineReader(engineLibrary);
    const std::vector<Track> engineTracks = engineReader.readTracks();
    std::map<std::string, const Track *> engineByPath;
    for (const Track &track : engineTracks) {
        engineByPath.emplace(keyOf(track.filePath), &track);
    }
    const Track *rb = nullptr;
    const Track *en = nullptr;
    for (const Track &track : rekordboxTracks) {
        const auto match = engineByPath.find(keyOf(track.filePath));
        if (domain::hasMp3Extension(track.filePath) && match != engineByPath.end()) {
            rb = &track;
            en = match->second;
            break;
        }
    }
    if (!rb) {
        std::cerr << "no MP3 in both catalogs of the fixture with an Engine beat grid\n";
        return 1;
    }
    const std::string relative =
        pathToUtf8(pathFromUtf8(rb->filePath).lexically_normal().lexically_relative(stick.lexically_normal()));
    std::cout << "planting on " << relative << " (rekordbox id " << rb->sourceId << ", Engine id " << en->sourceId
              << ")\n";
    fs::create_directories(pathFromUtf8(rb->filePath).parent_path());
    fs::copy_file(clickFile, pathFromUtf8(rb->filePath), fs::copy_options::overwrite_existing);

    const auto leadIn = infrastructure::audio::readMp3LeadIn(rb->filePath);
    check(leadIn && domain::classify(*leadIn) == domain::Mp3Case::D, "the planted file is class D");
    const double offsetMs = leadIn ? domain::engineToRekordboxOffsetMs(*leadIn) : 0.0;

    // Engine raw: the writer's lead-in source answers nothing, so it
    // stores the positions as given. 5000 and 10000 ms are whole samples
    // at 44.1 kHz.
    infrastructure::engine::LibdjinteropEngineCueWriter engineWriter(engineLibrary);
    engineWriter.setLeadInSource([](const std::string &) { return std::optional<domain::Mp3LeadIn>{}; });
    engineWriter.writeHotCuesAtSampleRate(en->sourceId, {hot(1, 5000.0), hot(2, 10000.0)}, 44100.0);
    // rekordbox: pad A where Engine has it raw, pad B where a right
    // correction puts Engine's (to the ms rekordbox stores).
    infrastructure::rekordbox::RekordboxCueWriter rekordboxWriter(pioneer);
    rekordboxWriter.writeHotCues(rb->sourceId, {hot(1, 5000.0), hot(2, 10051.0)});

    // An Engine beat grid at 120 BPM whose first beat is at 100 ms. The
    // fixture's rows hold no sample rate libdjinterop can read, so the
    // grid reader takes the rate the BPM and the markers imply.
    {
        auto db = djinterop::engine::load_database(engineLibrary);
        auto row = db.track_by_id(std::stoll(en->sourceId));
        row->set_bpm(120.0);
        row->set_beatgrid({{0, 4410.0}, {64, 4410.0 + 64 * 22050.0}});
    }
    const std::optional<double> rekordboxFirstBeat = [&]() -> std::optional<double> {
        for (const auto &beat : infrastructure::rekordbox::readBeatGrid(pioneer, rb->sourceId)) {
            if (beat.timeMs >= 0.0) {
                return beat.timeMs;
            }
        }
        return std::nullopt;
    }();
    check(rekordboxFirstBeat.has_value(), "the fixture's rekordbox grid has a first beat");

    const auto before = snapshot(stick);
    const std::string output = runTool(tool, stick);
    std::cout << output;
    check(snapshot(stick) == before, "the stick is unchanged after click_positions");

    const std::vector<std::string> block = blockFor(output, relative);
    check(!block.empty(), "click_positions lists " + relative);
    char expectCase[64];
    std::snprintf(expectCase, sizeof expectCase, "  case D, predicted offset %.3f ms", offsetMs);
    check(!block.empty() && block.front() == expectCase, std::string("it says: ") + expectCase);

    const auto padA = residualOn(block, "A");
    const double expectA = (5000.0 - 5000.0) + offsetMs;
    check(padA && std::abs(*padA - expectA) < 0.0015,
          "pad A residual " + std::to_string(padA.value_or(NAN)) + " = planted Engine - rekordbox (0) + offset "
              + std::to_string(expectA));
    const auto padB = residualOn(block, "B");
    const double expectB = (10000.0 - 10051.0) + offsetMs;
    check(padB && std::abs(*padB - expectB) < 0.0015,
          "pad B residual " + std::to_string(padB.value_or(NAN)) + " = planted Engine - rekordbox (-51) + offset "
              + std::to_string(expectB));
    check(output.find("\n    +51 ms  1\n") != std::string::npos, "the histogram has pad A in its +51 ms row");

    // The first beat: Engine's 100 ms moved by the offset, minus rekordbox's.
    char expectBeat[160];
    std::snprintf(expectBeat, sizeof expectBeat,
                  "  first beat: rekordbox %.3f, Engine raw 100.000, corrected %.3f, residual %+.3f ms",
                  rekordboxFirstBeat.value_or(0.0), 100.0 + offsetMs, 100.0 + offsetMs - rekordboxFirstBeat.value_or(0.0));
    check(block.size() > 1 && block[1] == expectBeat, std::string("it says: ") + expectBeat);

    std::cout << (failures == 0 ? "PASS" : "FAIL") << "\n";
    return failures == 0 ? 0 : 1;
}
