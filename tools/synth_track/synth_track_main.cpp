// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Writes one synthetic track (synth_track.hpp) for listening, for an
// encoder, or for another analyser to try:
//
//   synth_track --list
//   synth_track --preset NAME [--out x.wav] [--grid x.tsv] [--seed N]
//
// The WAV is 16-bit mono at the preset's rate; the grid is the true one,
// one beat a line. Exit code 0 on success, 1 when a file cannot be
// written, 2 for a usage error.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "synth_track/synth_track.hpp"

using namespace seabass;

namespace
{

int usage()
{
    std::fprintf(stderr, "usage: synth_track --list\n"
                         "       synth_track --preset NAME [--out x.wav] [--grid x.tsv] [--seed N]\n");
    return 2;
}

}  // namespace

int main(int argc, char **argv)
{
    std::string preset;
    std::string out;
    std::string grid;
    std::string seed;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](std::string &into) {
            if (i + 1 >= argc) {
                return false;
            }
            into = argv[++i];
            return true;
        };
        if (arg == "--list") {
            list = true;
        } else if (arg == "--preset") {
            if (!value(preset)) {
                return usage();
            }
        } else if (arg == "--out") {
            if (!value(out)) {
                return usage();
            }
        } else if (arg == "--grid") {
            if (!value(grid)) {
                return usage();
            }
        } else if (arg == "--seed") {
            if (!value(seed)) {
                return usage();
            }
        } else {
            return usage();
        }
    }
    if (list) {
        for (const std::string &name : synth::synthPresetNames()) {
            const auto spec = synth::synthPreset(name);
            std::printf("%-24s %s, %d Hz, %s, %s\n", name.c_str(), spec->description.c_str(), spec->sampleRate,
                        synth::drumPatternName(spec->pattern), spec->keyName.c_str());
        }
        return 0;
    }
    if (preset.empty() || (out.empty() && grid.empty())) {
        return usage();
    }
    auto spec = synth::synthPreset(preset);
    if (!spec) {
        std::fprintf(stderr, "no preset %s (synth_track --list names them)\n", preset.c_str());
        return 2;
    }
    if (!seed.empty()) {
        char *end = nullptr;
        spec->seed = std::strtoull(seed.c_str(), &end, 10);
        if (end == seed.c_str() || *end != '\0') {
            return usage();
        }
    }
    std::string error;
    if (!out.empty()) {
        const synth::SynthTrack track = synth::renderSynthTrack(*spec);
        if (!synth::writeWav16(out, track.samples, track.sampleRate, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::printf("%s: %zu samples at %d Hz, %zu beats\n", out.c_str(), track.samples.size(), track.sampleRate,
                    track.beats.size());
    }
    if (!grid.empty()) {
        const auto beats = synth::synthGrid(*spec);
        if (!synth::writeGridTsv(grid, *spec, beats, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::printf("%s: %zu beats\n", grid.c_str(), beats.size());
    }
    return 0;
}
