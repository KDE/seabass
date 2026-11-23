// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// domain/seabass_grid_analyser on generated tracks, whose grids are known
// to the sample: the tempo to a few thousandths of a BPM, the beat within
// a few milliseconds and not between the beats, at 44.1 and at 48 kHz,
// through a breakdown and after a drumless intro; and what it says of
// audio that has no beat.

#include <cmath>
#include <cstdint>
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

// Audio with nothing regular in it: whatever the analyser makes of it, it
// comes back, and with beats that are times in the track, in order.
void nothingRegular()
{
    const int rate = 44100;
    std::uint32_t state = 2463534242u;
    const auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<double>(state) / 4294967296.0;
    };
    const auto sane = [rate](const GridAnalysis &analysis, std::size_t samples, const std::string &what) {
        check(analysis.error.empty(), what + ": not an error");
        const double lengthMs = 1000.0 * static_cast<double>(samples) / rate;
        bool ordered = true;
        for (std::size_t i = 0; i < analysis.beats.size(); ++i) {
            const double t = analysis.beats[i].timeMs;
            ordered = ordered && std::isfinite(t) && t >= 0.0 && t <= lengthMs
                && (i == 0 || t > analysis.beats[i - 1].timeMs);
        }
        check(ordered, what + ": beats in order and inside the track");
        check(analysis.beats.size() < samples / 4000, what + ": no more beats than a track can hold");
    };
    SeabassGridAnalyser analyser;

    // White noise.
    std::vector<float> noise(static_cast<std::size_t>(rate) * 30);
    for (float &sample : noise) {
        sample = static_cast<float>(next() - 0.5);
    }
    InMemoryPcmSource hiss(noise, rate);
    sane(analyser.analyse(hiss), noise.size(), "noise");

    // Thumps at times that follow no tempo, of every loudness, on a hum:
    // many seeds, since a search that ends on a slope is a matter of luck.
    for (int seed = 0; seed < 12; ++seed) {
        std::vector<float> thumps(static_cast<std::size_t>(rate) * 25, 0.0f);
        for (std::size_t i = 0; i < thumps.size(); ++i) {
            thumps[i] = 0.05f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 55.0 * static_cast<double>(i) / rate));
        }
        std::size_t at = static_cast<std::size_t>(next() * rate);
        while (at + 4000 < thumps.size()) {
            const double loud = 0.1 + 0.9 * next();
            for (std::size_t k = 0; k < 4000; ++k) {
                const double t = static_cast<double>(k) / rate;
                thumps[at + k] += static_cast<float>(loud * std::exp(-t * 40.0) * std::sin(2.0 * 3.14159265358979 * 70.0 * t));
            }
            at += static_cast<std::size_t>((0.13 + 0.9 * next() * next()) * rate);
        }
        InMemoryPcmSource irregular(thumps, rate);
        sane(analyser.analyse(irregular), thumps.size(), "thumps at no tempo, seed " + std::to_string(seed));
    }
}

// A kick on every beat whose three parts swell one after the other (the
// body at 250 Hz, then 100 Hz, then the sub at 50 Hz), and between the
// beats a tick: two milliseconds of 3 kHz. Folded onto the beat the tick
// is the taller spike, all of one band in a moment, and the kick the
// heavier hump, three bands over thirty milliseconds; the grid belongs on
// the kick.
void spikeBetweenTheBeats(double clickLoudness, const std::string &what)
{
    const int rate = 44100;
    const double period = 0.5;
    const int count = 80;
    const double pi = 3.14159265358979;
    std::vector<float> audio(static_cast<std::size_t>((count * period + 1.0) * rate), 0.0f);
    std::vector<Beat> truth;
    struct Part
    {
        double hz, delay, attack, decay, loud;
    };
    const Part parts[] = {{250.0, 0.000, 0.020, 0.08, 0.25}, {100.0, 0.012, 0.025, 0.12, 0.3}, {50.0, 0.025, 0.030, 0.20, 0.35}};
    for (int k = 0; k < count; ++k) {
        const double beat = 0.25 + k * period;
        truth.push_back({beat * 1000.0, k % 4 + 1});
        for (const Part &part : parts) {
            const auto from = static_cast<std::size_t>((beat + part.delay) * rate);
            for (std::size_t i = 0; i < static_cast<std::size_t>(0.4 * rate) && from + i < audio.size(); ++i) {
                const double t = static_cast<double>(i) / rate;
                audio[from + i] += static_cast<float>(part.loud * (1.0 - std::exp(-t / part.attack)) * std::exp(-t / part.decay)
                                                      * std::sin(2.0 * pi * part.hz * t));
            }
        }
        const auto click = static_cast<std::size_t>((beat + period / 2.0) * rate);
        const auto length = static_cast<std::size_t>(0.002 * rate);
        for (std::size_t i = 0; i < length && click + i < audio.size(); ++i) {
            const double t = static_cast<double>(i) / rate;
            const double shape = std::sin(pi * static_cast<double>(i) / static_cast<double>(length));
            audio[click + i] += static_cast<float>(clickLoudness * shape * std::sin(2.0 * pi * 3000.0 * t));
        }
    }
    InMemoryPcmSource source(audio, rate);
    SeabassGridAnalyser analyser;
    const GridAnalysis analysis = analyser.analyse(source);
    const BeatGridComparison c = compareBeatGrids(analysis.beats, truth);
    check(c.tempo == TempoRelation::Same, what + ": the tempo of the kicks");
    within(c.offset.medianMs, -40.0, 70.0, what + ": the grid on the kick, in ms from where it starts");
}

}  // namespace

int main()
{
    spikeBetweenTheBeats(0.15, "a tick between the beats");
    spikeBetweenTheBeats(0.5, "a loud tick between the beats");
    nothingRegular();
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
