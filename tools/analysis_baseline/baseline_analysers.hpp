// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The phase 2 baseline of docs/track-analysis-plan.md: Mixxx's beat and
// key analysis (mixxx_qm_plugins.hpp, mixxx_beatutils.hpp) behind the
// GridAnalyser port and a KeyAnalyser of the benchmark's own, so the
// benchmark scores them like any other analyser. For
// tools/analysis_benchmark and its tests only: built with
// -DSEABASS_ANALYSIS_BASELINE=ON, never linked into the app.

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "domain/grid_analyser.hpp"
#include "domain/pcm_source.hpp"

namespace seabass::baseline
{

// What the Queen Mary tracker made of a whole source: its beats in
// frames from sample 0, as they come.
struct QmBeats
{
    int sampleRate = 0;
    double totalFrames = 0.0;
    std::vector<double> beatFrames;
    std::string error;
};

// Reads `source` to the end in Mixxx's chunks and runs the tracker.
QmBeats trackQueenMaryBeats(domain::PcmSource &source);

// The names makeBaselineGridAnalyser knows, in report order:
//   qm-raw                    the tracker's beats as they come: a moving
//                             grid, as Mixxx would store with the fixed
//                             tempo assumption off, but not ironed
//   mixxx-constant            Mixxx's default: one constant grid from
//                             the ironed regions, its BPM rounded
//   mixxx-constant-unrounded  the same without the rounding step
// None of them finds a downbeat: every beatInBar is 0.
std::vector<std::string> baselineGridAnalyserNames();
std::unique_ptr<domain::GridAnalyser> makeBaselineGridAnalyser(const std::string &name);

// A beat list put into the grid Mixxx's constant method makes of it,
// every beat of that grid from the start of the track to totalFrames.
// Exposed for the tests of the port.
std::vector<domain::Beat> mixxxConstantGrid(const std::vector<double> &beatFrames, int sampleRate,
                                            double totalFrames, bool roundBpm, double *bpm = nullptr);

// --- Key ---

struct KeyAnalysis
{
    // As CamelotKey::parse reads it: "Am", "Db", "F#m". Empty when the
    // analyser found none.
    std::string key;
    // Empty when the analyser ran; see GridAnalysis::error.
    std::string error;
};

// One key per track. A port of the benchmark's own for now, not in
// src/domain, until phase 5 says what a key analyser must give.
class KeyAnalyser
{
public:
    virtual ~KeyAnalyser() = default;
    virtual std::string name() const = 0;
    virtual KeyAnalysis analyse(domain::PcmSource &source) = 0;
};

// "qm-key": qm-dsp's key mode detector as Mixxx configures it, reduced to
// one key per track as Mixxx does (the key that holds longest).
std::vector<std::string> baselineKeyAnalyserNames();
std::unique_ptr<KeyAnalyser> makeBaselineKeyAnalyser(const std::string &name);

// Mixxx's key numbers (1 to 12 C major to B major, 13 to 24 C minor to
// B minor) as key names; empty for 0 or anything else.
std::string chromaticKeyName(int key);

}  // namespace seabass::baseline
