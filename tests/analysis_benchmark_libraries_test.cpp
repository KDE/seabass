// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// analysis_benchmark libraries on a scratch stick built from the committed
// one_stick_two_catalogs fixture: one MP3 both catalogs hold gets the
// class D click file as its audio, and its Engine grid is planted as
// rekordbox's grid put on Engine's timeline (the file's lead-in taken
// off) and then moved 2 ms on. The tool must find exactly that: the row
// on the corrected timeline, rekordbox's grid moved by minus the lead-in
// offset, a median of 2 ms (to the rounding of rekordbox's whole
// milliseconds), the same tempo. With the audio out of reach the row
// says its lead-in is unknown and shows the offset uncorrected. And the
// stick is byte for byte as it was, with no scratch copy left behind.
//
// Usage: analysis_benchmark_libraries_test <one_stick_two_catalogs dir> <click mp3> <analysis_benchmark binary>

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
#include <djinterop/engine/v3/engine_library.hpp>

#include "domain/beat_grid_comparison.hpp"
#include "domain/mp3_lead_in.hpp"
#include "domain/track.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/rekordbox_beat_grid_reader.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass;
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

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << in.rdbuf();
    return bytes.str();
}

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

int runTool(const std::string &command)
{
#if defined(_WIN32)
    // cmd /c strips the first and last quote of a line that starts with
    // one, so the whole command gets one more pair for it to take.
    FILE *pipe = _popen(("\"" + command + " 2>NUL\"").c_str(), "r");
#else
    FILE *pipe = popen((command + " 2>/dev/null").c_str(), "r");
#endif
    if (!pipe) {
        return -1;
    }
    char buffer[4096];
    while (std::fgets(buffer, sizeof buffer, pipe)) {
        std::cout << buffer;
    }
#if defined(_WIN32)
    return _pclose(pipe);
#else
    return pclose(pipe);
#endif
}

std::string quoted(const std::string &text)
{
    return "\"" + text + "\"";
}

// The TSV line of one rekordbox track, by column name.
std::map<std::string, std::string> tsvRow(const fs::path &tsv, const std::string &rekordboxId)
{
    std::ifstream in(tsv);
    std::string header;
    std::getline(in, header);
    const auto split = [](const std::string &line) {
        std::vector<std::string> fields;
        std::istringstream stream(line);
        std::string field;
        while (std::getline(stream, field, '\t')) {
            fields.push_back(field);
        }
        return fields;
    };
    const std::vector<std::string> names = split(header);
    std::string line;
    while (std::getline(in, line)) {
        const std::vector<std::string> fields = split(line);
        if (!fields.empty() && fields.front() == rekordboxId) {
            std::map<std::string, std::string> row;
            for (std::size_t i = 0; i < names.size() && i < fields.size(); ++i) {
                row[names[i]] = fields[i];
            }
            return row;
        }
    }
    return {};
}

double number(const std::map<std::string, std::string> &row, const std::string &column)
{
    const auto it = row.find(column);
    return it == row.end() ? NAN : std::stod(it->second);
}

std::string keyOf(const std::string &filePath)
{
    return pathToUtf8(pathFromUtf8(filePath).lexically_normal());
}

bool noScratchLeft(const fs::path &dir)
{
    for (const auto &entry : fs::directory_iterator(dir)) {
        if (pathToUtf8(entry.path().filename()).rfind("analysis-benchmark-", 0) == 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::cerr << "usage: analysis_benchmark_libraries_test <one_stick_two_catalogs dir> <click mp3> "
                     "<analysis_benchmark>\n";
        return 2;
    }
    const fs::path fixture = pathFromUtf8(argv[1]);
    const fs::path clickFile = pathFromUtf8(argv[2]);
    const std::string tool = argv[3];

    const fs::path work = testing::scratchRoot() / "analysis_benchmark_libraries_test";
    fs::remove_all(work);
    const fs::path stick = work / "stick";
    const fs::path scratch = work / "scratch";
    const fs::path nowhere = work / "no-audio-here";
    fs::create_directories(stick);
    fs::create_directories(scratch);
    fs::create_directories(nowhere);
    fs::copy(fixture / "rekordbox", stick / "PIONEER", fs::copy_options::recursive);
    fs::copy(fixture / "engine", stick / "Engine Library", fs::copy_options::recursive);
    const std::string pioneer = pathToUtf8(stick / "PIONEER");
    const std::string engineLibrary = pathToUtf8(stick / "Engine Library");

    // The first MP3 both catalogs hold that has a rekordbox grid.
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
    std::vector<domain::Beat> rekordboxGrid;
    for (const Track &track : rekordboxTracks) {
        const auto match = engineByPath.find(keyOf(track.filePath));
        if (domain::hasMp3Extension(track.filePath) && match != engineByPath.end()) {
            rekordboxGrid = infrastructure::rekordbox::readBeatGrid(pioneer, track.sourceId);
            if (rekordboxGrid.size() >= 16) {
                rb = &track;
                en = match->second;
                break;
            }
        }
    }
    if (!rb) {
        std::cerr << "no MP3 in both catalogs of the fixture with a rekordbox beat grid\n";
        return 1;
    }
    fs::create_directories(pathFromUtf8(rb->filePath).parent_path());
    fs::copy_file(clickFile, pathFromUtf8(rb->filePath), fs::copy_options::overwrite_existing);
    const auto leadIn = infrastructure::audio::readMp3LeadIn(rb->filePath);
    check(leadIn && domain::classify(*leadIn) == domain::Mp3Case::D, "the planted file is class D");
    const double offsetMs = leadIn ? domain::engineToRekordboxOffsetMs(*leadIn) : 0.0;

    const domain::GridShape shape = domain::gridShape(rekordboxGrid);
    check(shape.kind == domain::GridShapeKind::Constant, "the fixture's rekordbox grid is constant");
    // rekordbox's line, on Engine's timeline, 2 ms later: as Engine
    // stores a grid, in samples (the fixture's rows hold no rate, so the
    // reader takes the one the tempo and the markers imply: 44.1 kHz with
    // this tempo).
    constexpr double ResidualMs = 2.0;
    constexpr double Rate = 44100.0;
    const double beatMs = 60000.0 / shape.bpm;
    const double firstMs = rekordboxGrid.front().timeMs - offsetMs + ResidualMs;
    const int last = static_cast<int>(rekordboxGrid.size()) - 1;
    {
        auto db = djinterop::engine::load_database(engineLibrary);
        auto row = db.track_by_id(std::stoll(en->sourceId));
        row->set_bpm(shape.bpm);
        row->set_beatgrid({{0, firstMs * Rate / 1000.0}, {last, (firstMs + last * beatMs) * Rate / 1000.0}});
    }
    // Engine's own analysis: without the rekordbox import's mark.
    djinterop::engine::v3::engine_library::load(engineLibrary)
        .performance_data()
        .set_third_party_source_id(std::stoll(en->sourceId), std::nullopt);

    const auto before = snapshot(stick);
    const fs::path tsv = work / "out.tsv";
    const int status = runTool(quoted(tool) + " libraries " + quoted(pathToUtf8(stick)) + " --tsv "
                               + quoted(pathToUtf8(tsv)) + " --scratch " + quoted(pathToUtf8(scratch)));
    check(status == 0, "analysis_benchmark libraries exits 0");
    check(snapshot(stick) == before, "the stick is unchanged");
    check(noScratchLeft(scratch), "no scratch copy left behind");

    const auto row = tsvRow(tsv, rb->sourceId);
    check(!row.empty(), "the TSV has a line for rekordbox id " + rb->sourceId);
    check(row.count("timeline") && row.at("timeline") == "mp3-corrected", "the row is on the corrected timeline");
    check(row.count("import_mark") && row.at("import_mark") == "no", "the row carries no import mark");
    check(std::abs(number(row, "shift_ms") + offsetMs) < 0.0006,
          "rekordbox's grid moved by " + std::to_string(number(row, "shift_ms")) + ", minus the offset "
              + std::to_string(offsetMs));
    check(std::abs(number(row, "median_ms") - ResidualMs) <= 0.6,
          "median " + std::to_string(number(row, "median_ms")) + " is the planted 2 ms");
    check(row.count("tempo") && row.at("tempo") == "same", "same tempo");
    check(row.count("engine_shape") && row.at("engine_shape") == "constant", "Engine's grid constant");
    check(row.count("agreed") && row.at("agreed") == "yes", "the row is in the agreed subset");

    // The audio out of reach: the lead-in is unknown, nothing is moved,
    // and the offset shows uncorrected.
    const fs::path tsvUncorrected = work / "out-uncorrected.tsv";
    const int statusUncorrected =
        runTool(quoted(tool) + " libraries " + quoted(pathToUtf8(stick)) + " --tsv " + quoted(pathToUtf8(tsvUncorrected))
                + " --scratch " + quoted(pathToUtf8(scratch)) + " --audio-root " + quoted(pathToUtf8(nowhere)));
    check(statusUncorrected == 0, "analysis_benchmark libraries --audio-root exits 0");
    const auto raw = tsvRow(tsvUncorrected, rb->sourceId);
    check(raw.count("timeline") && raw.at("timeline") == "mp3-lead-in-unknown", "without the audio: lead-in unknown");
    check(number(raw, "shift_ms") == 0.0, "without the audio: nothing moved");
    check(std::abs(number(raw, "median_ms") - (ResidualMs - offsetMs)) <= 0.6,
          "without the audio: median " + std::to_string(number(raw, "median_ms")) + " is 2 ms less the offset");
    check(raw.count("agreed") && raw.at("agreed") == "no", "without the audio: not agreed");

    fs::remove_all(work);
    std::cout << (failures == 0 ? "PASS" : "FAIL") << "\n";
    return failures == 0 ? 0 : 1;
}
