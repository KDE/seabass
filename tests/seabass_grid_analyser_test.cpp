// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// domain/seabass_grid_analyser on generated tracks, whose grids are known
// to the sample: the tempo to a few thousandths of a BPM, the beat within
// a few milliseconds and not between the beats, at 44.1 and at 48 kHz,
// through a breakdown and after a drumless intro; and what it says of
// audio that has no beat.

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "domain/beat_grid_comparison.hpp"
#include "domain/seabass_grid_analyser.hpp"
#include "synth_track/synth_track.hpp"

using namespace seabass;
using namespace seabass::domain;

namespace
{
int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

void within(double actual, double low, double high, const std::string &what)
{
    if (!(actual >= low && actual <= high)) {
        std::cerr << "FAILED: " << what << ": " << actual << ", expected " << low << " to " << high << "\n";
        ++failures;
    }
}

GridAnalysis analysed(const synth::SynthTrack &track)
{
    InMemoryPcmSource source(track.samples, track.sampleRate);
    SeabassGridAnalyser analyser;
    return analyser.analyse(source);
}

// A preset whose tempo does not move: the grid found is one line at the
// true tempo, on the true beats.
void constantPreset(const std::string &name, double trueBpm)
{
    const synth::SynthTrack track = synth::renderSynthTrack(*synth::synthPreset(name));
    const GridAnalysis analysis = analysed(track);
    check(analysis.error.empty(), name + ": no error");
    const BeatGridComparison c = compareBeatGrids(analysis.beats, track.beats);
    check(c.tempo == TempoRelation::Same, name + ": the same tempo as the truth");
    check(c.candidateShape.kind == GridShapeKind::Constant, name + ": one constant grid");
    within(c.candidateShape.bpm, trueBpm - 0.01, trueBpm + 0.01, name + ": the tempo to a hundredth of a BPM");
    // Not between the beats, and near them: the smoothed envelope rises
    // fastest a few milliseconds before the first sample of the kick,
    // which is where the grid sits until the beat is refined on the
    // samples.
    within(c.offset.medianMs, -6.0, 0.0, name + ": the beat, in ms from the true one");
    within(c.offset.madMs, 0.0, 0.5, name + ": every beat the same distance from the true one");
    check(c.drift.known, name + ": the drift is known");
    within(c.drift.ms, -2.0, 2.0, name + ": no drift over the track");
    within(c.cover, 0.98, 1.0, name + ": beats from the start of the track to its end");
    within(analysis.confidence, 0.5, 1.0, name + ": most windows agree with the grid");
}

void noBeat()
{
    // Silence, and a steady tone: nothing repeats, and no grid is made up.
    const int rate = 44100;
    std::vector<float> silence(static_cast<std::size_t>(rate) * 20, 0.0f);
    InMemoryPcmSource quiet(silence, rate);
    SeabassGridAnalyser analyser;
    const GridAnalysis fromSilence = analyser.analyse(quiet);
    check(fromSilence.error.empty(), "silence: not an error");
    check(fromSilence.beats.empty(), "silence: no beats");

    std::vector<float> tone(static_cast<std::size_t>(rate) * 20);
    for (std::size_t i = 0; i < tone.size(); ++i) {
        tone[i] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 220.0 * static_cast<double>(i) / rate));
    }
    InMemoryPcmSource steady(tone, rate);
    const GridAnalysis fromTone = analyser.analyse(steady);
    check(fromTone.error.empty(), "a steady tone: not an error");
    check(fromTone.beats.empty(), "a steady tone: no beats");

    // Two seconds of audio are too few beats to say anything.
    std::vector<float> brief(static_cast<std::size_t>(rate) * 2, 0.1f);
    InMemoryPcmSource tooShort(brief, rate);
    check(analyser.analyse(tooShort).beats.empty(), "two seconds: no beats");

    // A source that fails is an error, not a track without beats.
    InMemoryPcmSource broken({}, 0);
    check(!analyser.analyse(broken).error.empty(), "a source without a rate: an error");
}

}  // namespace

int main()
{
    constantPreset("house-124", 124.0);
    constantPreset("house-124-48k", 124.0);
    // A tempo that is no whole number stays what it is.
    constantPreset("trance-137.98", 137.98);
    constantPreset("long-breakdown-126", 126.0);
    constantPreset("drumless-intro-128", 128.0);
    noBeat();
    if (failures > 0) {
        std::cerr << failures << " check(s) FAILED\n";
        return 1;
    }
    std::cout << "seabass_grid_analyser_test: all checks passed\n";
    return 0;
}
