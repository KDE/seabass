// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Hardware probe for Copy Beat Grids: puts a grid of the caller's making
// on one Engine row, through the same writer the feature uses, so that a
// player can be asked a question rekordbox's own grids do not ask.
//
// A copied grid on a click track looks like Denon's own analysis: both
// found the same clicks. Whether the player plays by the grid Seabass
// wrote shows only when that grid is visibly somewhere else, half a beat
// late say, or changing tempo halfway through a track that does not.
//
// usage: beat_grid_probe <Engine Library dir> <track id> --bpm B --first-ms F --until-ms U
//            [--bpm2 B2 --change-at-ms T]         a tempo change at T
//            [--unanalysed fill|fill-lock --rate HZ --duration-ms D]
//            [--overwrite]
//
// Times are on Engine's timeline: no MP3 lead-in shift is applied. The
// caller backs the library up; this writes straight to it.

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "domain/beat_grid.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"

using namespace seabass;

namespace
{
const char *words(application::BeatGridWriteResult::Outcome outcome)
{
    using Outcome = application::BeatGridWriteResult::Outcome;
    switch (outcome) {
    case Outcome::Written:
        return "written";
    case Outcome::AlreadySame:
        return "already the same";
    case Outcome::NoSourceGrid:
        return "no source grid";
    case Outcome::RowNotAnalysed:
        return "row not analysed";
    case Outcome::RowLocked:
        return "row locked";
    case Outcome::RowEditedOnEngine:
        return "row edited on Engine";
    case Outcome::NoSampleRate:
        return "no sample rate";
    case Outcome::UnsupportedSchema:
        return "unsupported schema";
    }
    return "unknown";
}
}  // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::cerr << "usage: beat_grid_probe <Engine Library dir> <track id> --bpm B --first-ms F --until-ms U\n"
                     "           [--bpm2 B2 --change-at-ms T] [--unanalysed fill|fill-lock --rate HZ --duration-ms D]\n"
                     "           [--overwrite]\n";
        return 2;
    }
    const std::string library = argv[1];
    const std::string trackId = argv[2];
    std::map<std::string, std::string> values;
    bool overwrite = false;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--overwrite") {
            overwrite = true;
        } else if (arg.rfind("--", 0) == 0 && i + 1 < argc) {
            values[arg] = argv[++i];
        } else {
            std::cerr << "beat_grid_probe: what is \"" << arg << "\"?\n";
            return 2;
        }
    }
    const auto number = [&values](const std::string &name, double otherwise) {
        const auto found = values.find(name);
        return found == values.end() ? otherwise : std::atof(found->second.c_str());
    };
    const double bpm = number("--bpm", 0.0);
    const double firstMs = number("--first-ms", 0.0);
    const double untilMs = number("--until-ms", 0.0);
    const double bpm2 = number("--bpm2", 0.0);
    const double changeAtMs = number("--change-at-ms", 0.0);
    if (!(bpm > 0.0) || !(untilMs > firstMs)) {
        std::cerr << "beat_grid_probe: --bpm and --until-ms are needed\n";
        return 2;
    }

    // Every beat written out, as rekordbox would, the bar starting on the
    // first one.
    std::vector<domain::Beat> beats;
    double at = firstMs;
    for (int i = 0; at <= untilMs; ++i) {
        beats.push_back({at, i % 4 + 1});
        at += 60000.0 / (bpm2 > 0.0 && at >= changeAtMs ? bpm2 : bpm);
    }

    application::BeatGridWriteOptions options;
    options.overwriteEngineEdits = overwrite;
    const std::string unanalysed = values.count("--unanalysed") ? values["--unanalysed"] : "skip";
    if (unanalysed == "fill") {
        options.unanalysed = application::UnanalysedRowGrid::Fill;
    } else if (unanalysed == "fill-lock") {
        options.unanalysed = application::UnanalysedRowGrid::FillAndLock;
    } else if (unanalysed != "skip") {
        std::cerr << "beat_grid_probe: --unanalysed takes fill or fill-lock\n";
        return 2;
    }
    if (values.count("--rate")) {
        options.fileSampleRateHz = number("--rate", 0.0);
    }
    if (values.count("--duration-ms")) {
        options.fileDurationMs = number("--duration-ms", 0.0);
    }

    try {
        infrastructure::engine::LibdjinteropEngineCueWriter writer(library);
        // The times given are Engine's already.
        writer.setLeadInSource({});
        const application::BeatGridWriteResult result = writer.writeBeatGrid(trackId, beats, options);
        std::cout << "track " << trackId << ": " << words(result.outcome) << ", " << beats.size() << " beat(s) given, "
                  << result.markers << " marker(s), fingerprint " << result.gridFingerprint << "\n";
        return result.outcome == application::BeatGridWriteResult::Outcome::Written
                || result.outcome == application::BeatGridWriteResult::Outcome::AlreadySame
            ? 0
            : 1;
    } catch (const std::exception &e) {
        std::cerr << "beat_grid_probe: " << e.what() << "\n";
        return 1;
    }
}
