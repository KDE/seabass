// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// domain/seabass_grid_analyser on generated tracks, whose grids are known
// to the sample: the tempo to a few thousandths of a BPM, the beat within
// a few milliseconds and not between the beats, at 44.1 and at 48 kHz,
// through a breakdown and after a drumless intro; and what it says of
// audio that has no beat.

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "domain/audio_analysis.hpp"
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
    check(!analysis.followsTempo, name + ": said to be one straight grid");
}

// A preset whose tempo moves: the grid found moves with it. `within8` and
// `within20` are the least shares of the true beats that must have one of
// ours within 8 and 20 ms.
void movingPreset(const std::string &name, double within8, double within20)
{
    const synth::SynthTrack track = synth::renderSynthTrack(*synth::synthPreset(name));
    const GridAnalysis analysis = analysed(track);
    check(analysis.error.empty(), name + ": no error");
    // Against the truth, the followed grid put back by what it is set
    // early by: that is calibration against rekordbox and not a distance
    // from the beat.
    const BeatGridComparison c = compareBeatGrids(analysis.beats, track.beats, SeabassGridAnalyser::FollowedGridEarlierMs);
    check(c.tempo == TempoRelation::Same, name + ": the same tempo as the truth, beat for beat");
    // And as it is given out, it is that much earlier.
    const BeatGridComparison given = compareBeatGrids(analysis.beats, track.beats);
    within(given.offset.medianMs - c.offset.medianMs, -SeabassGridAnalyser::FollowedGridEarlierMs - 1e-6,
           -SeabassGridAnalyser::FollowedGridEarlierMs + 1e-6, name + ": given out early by what it is calibrated by");
    check(c.candidateShape.kind == GridShapeKind::Moving, name + ": a grid that moves");
    check(analysis.followsTempo, name + ": said to follow the tempo");
    within(c.hits.within8Ms, within8, 1.0, name + ": true beats with one of ours within 8 ms");
    within(c.hits.within20Ms, within20, 1.0, name + ": true beats with one of ours within 20 ms");
    check(c.drift.known, name + ": the drift is known");
    within(c.drift.ms, -10.0, 10.0, name + ": it does not run away from the truth");
    within(c.cover, 0.97, 1.0, name + ": beats from the start of the track to its end");
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

// A kick: a body at 60 Hz that dies away in a seventh of a second, and a
// click on its first samples.
void addKick(std::vector<float> &audio, int rate, double at, double loud = 0.8)
{
    const double pi = 3.14159265358979;
    const auto from = static_cast<std::size_t>(at * rate);
    for (std::size_t i = 0; i < static_cast<std::size_t>(0.4 * rate) && from + i < audio.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const double body = std::exp(-t / 0.15) * std::sin(2.0 * pi * 60.0 * t);
        const double click = std::exp(-t / 0.004) * std::sin(2.0 * pi * 1800.0 * t);
        audio[from + i] += static_cast<float>(loud * (0.8 * body + 0.3 * click));
    }
}

// A chord stab: 400 and 800 Hz for a twelfth of a second.
void addStab(std::vector<float> &audio, int rate, double at, double loud = 0.5)
{
    const double pi = 3.14159265358979;
    const auto from = static_cast<std::size_t>(at * rate);
    for (std::size_t i = 0; i < static_cast<std::size_t>(0.2 * rate) && from + i < audio.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        const double shape = (1.0 - std::exp(-t / 0.002)) * std::exp(-t / 0.08);
        audio[from + i] += static_cast<float>(loud * shape * 0.5 * (std::sin(2.0 * pi * 400.0 * t) + std::sin(2.0 * pi * 800.0 * t)));
    }
}

// A tone of `hz` that swells in `attack` seconds and dies away in `decay`.
void addNote(std::vector<float> &audio, int rate, double at, double hz, double loud, double attack, double decay)
{
    const double pi = 3.14159265358979;
    const auto from = static_cast<std::size_t>(at * rate);
    for (std::size_t i = 0; i < static_cast<std::size_t>(6.0 * decay * rate) && from + i < audio.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        audio[from + i] += static_cast<float>(loud * (1.0 - std::exp(-t / attack)) * std::exp(-t / decay)
                                              * std::sin(2.0 * pi * hz * t));
    }
}

// House as it is made: on the beat a kick (its body at 100 Hz, a knock at
// 700 Hz, a click at 3 kHz), between the beats the bass at 45 Hz with a
// chord at 250 Hz and a little at 700 Hz and 3 kHz. Counting every band
// alike, the bass and its chord outweigh the kick; the grid belongs on
// the kick all the same, which is why the band under 70 Hz has no say in
// where the beat is.
void bassBetweenTheBeats()
{
    const int rate = 44100;
    const double period = 60.0 / 126.0;
    const int count = 160;
    std::vector<float> audio(static_cast<std::size_t>((count * period + 1.0) * rate), 0.0f);
    std::vector<Beat> truth;
    for (int k = 0; k < count; ++k) {
        const double beat = 0.3 + k * period;
        truth.push_back({beat * 1000.0, k % 4 + 1});
        addNote(audio, rate, beat, 100.0, 0.5, 0.004, 0.09);
        addNote(audio, rate, beat, 700.0, 0.11, 0.002, 0.03);
        addNote(audio, rate, beat, 3000.0, 0.09, 0.001, 0.008);
        const double between = beat + period / 2.0;
        addNote(audio, rate, between, 45.0, 0.6, 0.006, 0.12);
        addNote(audio, rate, between, 250.0, 0.3, 0.004, 0.08);
        addNote(audio, rate, between, 700.0, 0.09, 0.002, 0.03);
        addNote(audio, rate, between, 3000.0, 0.06, 0.001, 0.008);
    }
    InMemoryPcmSource source(audio, rate);
    SeabassGridAnalyser analyser;
    const GridAnalysis analysis = analyser.analyse(source);
    const BeatGridComparison c = compareBeatGrids(analysis.beats, truth);
    check(c.tempo == TempoRelation::Same, "bass between the beats: the tempo");
    within(c.offset.medianMs, -20.0, 20.0, "bass between the beats: the grid on the kick");
}

// A kick on every beat and nothing between them, at tempos whose half
// period repeats nowhere: the lag that scores best by its multiples is
// then one the track does not repeat at itself, and the grid must still
// be made.
void slowAndSparse()
{
    const int rate = 44100;
    for (const double bpm : {62.0, 70.0, 76.0}) {
        std::vector<float> audio(static_cast<std::size_t>(rate) * 60, 0.0f);
        std::vector<Beat> truth;
        int k = 0;
        for (double at = 0.3; at < 59.0; at += 60.0 / bpm, ++k) {
            addKick(audio, rate, at);
            truth.push_back({at * 1000.0, k % 4 + 1});
        }
        InMemoryPcmSource source(audio, rate);
        // 62 BPM is under the default range, which would give it out as
        // 124; in a range that holds it, it is 62.
        SeabassGridAnalyser analyser(bpm < 70.0 ? TempoRange{58.0, 115.0} : TempoRange{});
        const GridAnalysis analysis = analyser.analyse(source);
        const std::string name = "a kick alone at " + std::to_string(static_cast<int>(bpm)) + " BPM";
        check(!analysis.beats.empty(), name + ": a grid");
        const BeatGridComparison c = compareBeatGrids(analysis.beats, truth);
        check(c.tempo == TempoRelation::Same, name + ": its tempo");
        within(c.offset.medianMs, -15.0, 15.0, name + ": on the kick");
    }
}

// The tempo range decides between a tempo and its double: drum and bass,
// a kick and a snare taking turns 345 ms apart, is at 174 BPM in a range
// that holds 174 and at 87 in one that does not, and the same audio never
// comes out at a tempo outside the range it was given.
void tempoRanges()
{
    const int rate = 44100;
    const double period = 60.0 / 174.0;
    const int count = 260;
    std::vector<float> audio(static_cast<std::size_t>((count * period + 1.0) * rate), 0.0f);
    std::vector<Beat> truth;
    for (int k = 0; k < count; ++k) {
        const double beat = 0.3 + k * period;
        truth.push_back({beat * 1000.0, k % 4 + 1});
        if (k % 2 == 0) {
            addKick(audio, rate, beat);
        } else {
            addNote(audio, rate, beat, 220.0, 0.4, 0.002, 0.05);
            addNote(audio, rate, beat, 2500.0, 0.2, 0.001, 0.03);
        }
    }
    struct Case
    {
        TempoRange range;
        TempoRelation expected;
        const char *what;
    };
    const std::array<Case, 6> cases{{
        {{88.0, 175.0}, TempoRelation::Same, "88 to 175: 174"},
        {{98.0, 195.0}, TempoRelation::Same, "98 to 195: 174"},
        {{58.0, 115.0}, TempoRelation::Half, "58 to 115: 87"},
        {{45.0, 90.0}, TempoRelation::Half, "45 to 90: 87"},
        // Narrower than an octave: widened at the top, to 100 to 200.
        {{100.0, 110.0}, TempoRelation::Same, "100 to 110, widened: 174"},
        // No range at all: the default one, in which both 87 and 174 lie
        // and the answer is one of the two.
        {{0.0, -1.0}, TempoRelation::Unknown, "no range: the default"},
    }};
    for (const Case &c : cases) {
        InMemoryPcmSource source(audio, rate);
        SeabassGridAnalyser analyser(c.range);
        const GridAnalysis analysis = analyser.analyse(source);
        const BeatGridComparison compared = compareBeatGrids(analysis.beats, truth);
        if (c.expected == TempoRelation::Unknown) {
            check(analyser.tempoRange().lowBpm == 70.0 && analyser.tempoRange().highBpm == 180.0, c.what);
            check(compared.tempo == TempoRelation::Same || compared.tempo == TempoRelation::Half,
                  std::string(c.what) + ": 87 or 174");
        } else {
            check(compared.tempo == c.expected,
                  std::string("tempo range ") + c.what + ", was " + tempoRelationName(compared.tempo));
        }
    }
    // In a range of one octave no tempo is preferred for being near 128:
    // a low thud every second and a high tick every two thirds of one,
    // with the thud a little the stronger in the fold, is at 60 BPM in a
    // range of 45 to 90, where 90 would win on nearness to 128 alone.
    {
        std::vector<float> slow(static_cast<std::size_t>(rate) * 120, 0.0f);
        std::vector<Beat> thuds;
        int k = 0;
        for (double at = 0.4; at < 118.0; at += 1.0, ++k) {
            addNote(slow, rate, at, 50.0, 0.5, 0.004, 0.09);
            addNote(slow, rate, at, 100.0, 0.5, 0.004, 0.09);
            thuds.push_back({at * 1000.0, k % 4 + 1});
        }
        for (double at = 0.4; at < 118.0; at += 2.0 / 3.0) {
            addNote(slow, rate, at, 800.0, 0.3, 0.002, 0.03);
            addNote(slow, rate, at, 3000.0, 0.3, 0.001, 0.02);
        }
        InMemoryPcmSource source(slow, rate);
        SeabassGridAnalyser analyser(TempoRange{45.0, 90.0});
        const BeatGridComparison compared = compareBeatGrids(analyser.analyse(source).beats, thuds);
        check(compared.tempo == TempoRelation::Same,
              std::string("45 to 90, a thud at 60 and a tick at 90: 60, was ") + tempoRelationName(compared.tempo));
    }

    // A range none of the rhythm's own levels lies in: the tempo is
    // brought into it by octaves all the same. A kick at 170 BPM in a
    // range of 40 to 80 has a beat on every third or every fourth kick
    // (57 or 42 BPM; the kicks alone do not say which), not on every
    // second (85).
    {
        std::vector<float> fast(static_cast<std::size_t>(rate) * 120, 0.0f);
        for (double at = 0.3; at < 118.0; at += 60.0 / 170.0) {
            addKick(fast, rate, at);
        }
        InMemoryPcmSource source(fast, rate);
        SeabassGridAnalyser analyser(TempoRange{40.0, 80.0});
        const GridAnalysis analysis = analyser.analyse(source);
        check(analysis.beats.size() > 40, "40 to 80, a kick at 170: a grid");
        if (analysis.beats.size() > 40) {
            const double gap = (analysis.beats.back().timeMs - analysis.beats.front().timeMs)
                / static_cast<double>(analysis.beats.size() - 1);
            within(gap, 750.0, 1500.0, "40 to 80, a kick at 170: a tempo in the range");
            const double kicks = gap / (60000.0 / 170.0);
            within(kicks - std::round(kicks), -0.01, 0.01, "40 to 80, a kick at 170: a whole number of kicks to the beat");
        }
        // A steady kick is a sure grid, in a slow range as in any.
        check(analysis.confidence > 0.8, "40 to 80, a kick at 170: sure of it");
    }

    const TempoRange widened = SeabassGridAnalyser(TempoRange{100.0, 110.0}).tempoRange();
    check(widened.lowBpm == 100.0 && widened.highBpm == 200.0, "a range under an octave is widened at the top");
    const TempoRange point = SeabassGridAnalyser(TempoRange{100.0, 100.0}).tempoRange();
    check(point.lowBpm == 100.0 && point.highBpm == 200.0, "a range of one tempo is widened as well");
    const TempoRange lowered = SeabassGridAnalyser(TempoRange{200.0, 250.0}).tempoRange();
    check(lowered.lowBpm == 120.0 && lowered.highBpm == 240.0, "a range too high has its bottom brought down");
    const TempoRange clamped = SeabassGridAnalyser(TempoRange{5.0, 1000.0}).tempoRange();
    check(clamped.lowBpm == 40.0 && clamped.highBpm == 240.0, "a range is kept between 40 and 240 BPM");
}

// A tempo that moves is followed in a slow range and in a fast one as in
// the default: a kick that speeds up from `fromBpm` to `toBpm` over two
// and a half minutes, in a range that holds both and not their doubles.
void movingInARange(double fromBpm, double toBpm, TempoRange range, const std::string &what)
{
    const int rate = 44100;
    const double seconds = 150.0;
    std::vector<float> audio(static_cast<std::size_t>(rate * (seconds + 2.0)), 0.0f);
    std::vector<Beat> truth;
    int k = 0;
    for (double at = 0.5; at < seconds;) {
        addKick(audio, rate, at);
        truth.push_back({at * 1000.0, k++ % 4 + 1});
        at += 60.0 / (fromBpm + (toBpm - fromBpm) * at / seconds);
    }
    InMemoryPcmSource source(audio, rate);
    SeabassGridAnalyser analyser(range);
    const GridAnalysis analysis = analyser.analyse(source);
    const BeatGridComparison c = compareBeatGrids(analysis.beats, truth, SeabassGridAnalyser::FollowedGridEarlierMs);
    check(c.tempo == TempoRelation::Same, what + ": the tempo, was " + tempoRelationName(c.tempo));
    check(c.candidateShape.kind == GridShapeKind::Moving, what + ": a grid that moves");
    within(c.hits.within20Ms, 0.9, 1.0, what + ": true beats with one of ours within 20 ms");
}

// A drummer at 96 BPM who speeds up by eight percent over the first half
// of the track and slows down again over the second, and is never quite
// steady from bar to bar either: a kick on one and three, a snare on two
// and four, a hat on every eighth, and around them what a band plays that
// is not on the drummer's grid (here: notes at moments of chance). Over
// the whole track his beats pass through every place in the beat, so the
// fold of the whole track is nearly flat at every period alike, and by it
// the tempo came out on the preference for 128 alone, at four thirds of
// 96; window by window the track folds sharpest at 96. And the grid
// follows him: the path through the onsets does where the windows lose
// him.
//
// With six quiet notes a second off the grid the grid is asked for as
// well as the tempo. With twenty as loud as the drums only the tempo is:
// there the beats are not found (one in ten), and it is the case in which
// the fold of the whole track has the tempo wrong.
void wanderingDrummer(double offTheGridASecond, double offTheGridLoud, bool theGridToo)
{
    const int rate = 44100;
    const double pi = 3.14159265358979;
    const double seconds = 160.0;
    std::vector<float> audio(static_cast<std::size_t>(rate * (seconds + 2.0)), 0.0f);
    std::vector<Beat> truth;
    int k = 0;
    for (double at = 0.5; at < seconds; ++k) {
        truth.push_back({at * 1000.0, k % 4 + 1});
        const double longWave = 1.0 - std::abs(2.0 * at / seconds - 1.0);
        const double period = 60.0 / (92.0 * (1.0 + 0.08 * longWave) * (1.0 + 0.03 * std::sin(2.0 * pi * at / 9.0)));
        if (k % 2 == 0) {
            addKick(audio, rate, at);
        } else {
            addNote(audio, rate, at, 220.0, 0.45, 0.002, 0.06);
            addNote(audio, rate, at, 1800.0, 0.3, 0.001, 0.04);
        }
        addNote(audio, rate, at, 3500.0, 0.12, 0.001, 0.012);
        addNote(audio, rate, at + period / 2.0, 3500.0, 0.12, 0.001, 0.012);
        at += period;
    }
    // Chance that is the same on every machine: a standard library's
    // distributions are its own.
    std::uint32_t state = 88172645u;
    const auto chance = [&state]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<double>(state) / 4294967296.0;
    };
    for (int i = 0; i < static_cast<int>(seconds * offTheGridASecond); ++i) {
        const double at = 0.5 + (seconds - 0.5) * chance();
        const double loud = offTheGridLoud * (0.3 + 0.7 * chance());
        addNote(audio, rate, at, std::array<double, 4>{110.0, 260.0, 800.0, 2400.0}[static_cast<std::size_t>(i % 4)], loud, 0.003, 0.05);
    }
    InMemoryPcmSource source(audio, rate);
    SeabassGridAnalyser analyser;
    const GridAnalysis analysis = analyser.analyse(source);
    const BeatGridComparison c = compareBeatGrids(analysis.beats, truth, SeabassGridAnalyser::FollowedGridEarlierMs);
    const std::string name = theGridToo ? "a wandering drummer at 96" : "a wandering drummer at 96 among much off his grid";
    check(c.tempo == TempoRelation::Same, name + ": the tempo, was " + tempoRelationName(c.tempo));
    if (theGridToo) {
        check(c.candidateShape.kind == GridShapeKind::Moving, name + ": a grid that moves");
        within(c.hits.within20Ms, 0.9, 1.0, name + ": true beats with one of ours within 20 ms");
    }
}

// A drum machine at 124 BPM that goes to 124.5 half way (so that no
// straight grid explains the track and it is followed), with sections in
// which a stab between the beats is louder than the kick. Its tempo
// holds, so it is the windows that follow it, and they keep to the kick
// on over four fifths of the track. The path, which is for a tempo that
// wanders, sits on the stabs for the length of each such section, and has
// the kick on little over half.
void stabsBetweenTheBeatsForASection()
{
    const int rate = 44100;
    const int count = 320;
    std::vector<float> audio(static_cast<std::size_t>((count * 60.0 / 124.0 + 1.5) * rate), 0.0f);
    std::vector<Beat> truth;
    double beat = 0.3;
    for (int k = 0; k < count; ++k) {
        const double period = 60.0 / (k < count / 2 ? 124.0 : 124.5);
        truth.push_back({beat * 1000.0, k % 4 + 1});
        addKick(audio, rate, beat, 0.8);
        // Of every 80 beats the last 32 are such a section.
        if (k % 80 >= 48) {
            addStab(audio, rate, beat + period / 2.0, 1.0);
        }
        beat += period;
    }
    InMemoryPcmSource source(audio, rate);
    SeabassGridAnalyser analyser;
    const GridAnalysis analysis = analyser.analyse(source);
    const BeatGridComparison c = compareBeatGrids(analysis.beats, truth, SeabassGridAnalyser::FollowedGridEarlierMs);
    check(c.tempo == TempoRelation::Same, std::string("stabs between the beats for a section: the tempo, was ") + tempoRelationName(c.tempo));
    check(c.candidateShape.kind == GridShapeKind::Moving, "stabs between the beats for a section: followed");
    within(c.hits.within20Ms, 0.75, 1.0, "stabs between the beats for a section: the grid stays on the kick");
}

// A drummer at 100 BPM who falls behind for a quarter of the track, by up to
// 40 ms, and catches up again. One straight grid explains about four
// fifths of the track, more than the three quarters under which a track is
// followed outright. But the followed grid sits on the hits there too and
// the straight one does not, so the track is followed all the same.
void drummerWhoFallsBehindForAWhile()
{
    const int rate = 44100;
    const int count = 300;
    const double period = 0.6;
    const double pi = 3.14159265358979;
    std::vector<float> audio(static_cast<std::size_t>((count * period + 2.0) * rate), 0.0f);
    std::vector<Beat> truth;
    for (int k = 0; k < count; ++k) {
        double behind = 0.0;
        if (k >= 165 && k < 243) {
            const double through = std::sin(pi * (k - 165) / 78.0);
            behind = 0.040 * through * through;
        }
        const double at = 0.4 + k * period + behind;
        truth.push_back({at * 1000.0, k % 4 + 1});
        if (k % 2 == 0) {
            addKick(audio, rate, at);
        } else {
            addNote(audio, rate, at, 220.0, 0.45, 0.002, 0.06);
            addNote(audio, rate, at, 1800.0, 0.3, 0.001, 0.04);
        }
        addNote(audio, rate, at, 3500.0, 0.12, 0.001, 0.012);
        addNote(audio, rate, at + period / 2.0, 3500.0, 0.12, 0.001, 0.012);
    }
    InMemoryPcmSource source(audio, rate);
    SeabassGridAnalyser analyser;
    const GridAnalysis analysis = analyser.analyse(source);
    const BeatGridComparison c = compareBeatGrids(analysis.beats, truth, SeabassGridAnalyser::FollowedGridEarlierMs);
    const std::string name = "a drummer who falls behind for a while";
    check(c.tempo == TempoRelation::Same, name + ": the tempo, was " + tempoRelationName(c.tempo));
    check(c.candidateShape.kind == GridShapeKind::Moving, name + ": followed, though a straight grid explains most of it");
    within(c.hits.within20Ms, 0.95, 1.0, name + ": true beats with one of ours within 20 ms");
}

// A drum machine at 124 BPM with a stab between the beats, louder than the
// kick, on a fifth of the track. The windows of those sections have their
// peak on the stab, so the straight grid explains about four fifths of the
// track, as with the drummer above. But it sits on every kick and the
// followed grid sits no tighter: the track keeps its straight grid.
void stabsOnAFifthOfTheTrack()
{
    const int rate = 44100;
    const int count = 320;
    const double period = 60.0 / 124.0;
    std::vector<float> audio(static_cast<std::size_t>((count * period + 1.5) * rate), 0.0f);
    std::vector<Beat> truth;
    for (int k = 0; k < count; ++k) {
        const double beat = 0.3 + k * period;
        truth.push_back({beat * 1000.0, k % 4 + 1});
        addKick(audio, rate, beat, 0.8);
        // Of every 80 beats the last 16 are such a section.
        if (k % 80 >= 64) {
            addStab(audio, rate, beat + period / 2.0, 1.0);
        }
    }
    InMemoryPcmSource source(audio, rate);
    SeabassGridAnalyser analyser;
    const GridAnalysis analysis = analyser.analyse(source);
    const BeatGridComparison c = compareBeatGrids(analysis.beats, truth);
    const std::string name = "stabs between the beats on a fifth of the track";
    check(c.tempo == TempoRelation::Same, name + ": the tempo, was " + tempoRelationName(c.tempo));
    check(c.candidateShape.kind == GridShapeKind::Constant, name + ": one straight grid");
    within(c.offset.medianMs, -6.0, 0.0, name + ": the beat, in ms from the true one");
    within(analysis.confidence, 0.75, 0.85, name + ": the straight grid explains about four fifths of the track");
}

// A loop of seven seconds, shorter than the windows the tempo's candidates
// are compared in: it is one window, and its tempo is still read from the
// audio. (With no window at all every candidate scored nothing and the
// first one tried, half the period, was taken: 171 BPM for these 86.)
void shortLoop()
{
    const int rate = 44100;
    const double period = 0.7;
    std::vector<float> audio(static_cast<std::size_t>(rate * 7.5), 0.0f);
    std::vector<Beat> truth;
    int k = 0;
    for (double at = 0.2; at < 7.2; at += period, ++k) {
        addKick(audio, rate, at);
        truth.push_back({at * 1000.0, k % 4 + 1});
    }
    InMemoryPcmSource source(audio, rate);
    SeabassGridAnalyser analyser;
    const GridAnalysis analysis = analyser.analyse(source);
    check(!analysis.beats.empty(), "a loop of seven seconds: a grid");
    const BeatGridComparison c = compareBeatGrids(analysis.beats, truth);
    check(c.tempo == TempoRelation::Same, std::string("a loop of seven seconds: the tempo, was ") + tempoRelationName(c.tempo));
}

// Tones and hum: a level, held. No onsets, no grid; also where the tone is
// low enough that its square ripples under the envelope's smoothing, and
// where it has been through 16 bits.
void heldTones()
{
    const int rate = 44100;
    const double pi = 3.14159265358979;
    SeabassGridAnalyser analyser;
    for (const double hz : {50.0, 60.0, 110.0, 200.0, 220.0, 1000.0}) {
        for (const bool sixteenBits : {false, true}) {
            std::vector<float> tone(static_cast<std::size_t>(rate) * 20);
            for (std::size_t i = 0; i < tone.size(); ++i) {
                const double value = 0.5 * std::sin(2.0 * pi * hz * static_cast<double>(i) / rate);
                tone[i] = static_cast<float>(sixteenBits ? std::round(value * 32767.0) / 32767.0 : value);
            }
            InMemoryPcmSource source(tone, rate);
            const GridAnalysis analysis = analyser.analyse(source);
            check(analysis.beats.empty(), "a tone at " + std::to_string(static_cast<int>(hz)) + " Hz"
                                              + (sixteenBits ? " in 16 bits" : "") + ": no beats");
        }
    }
}

// Noise has nothing regular in it: whatever grid comes out, the analyser
// is not sure of it.
void noiseIsUnsure()
{
    const int rate = 44100;
    std::uint32_t state = 362436069u;
    std::vector<float> noise(static_cast<std::size_t>(rate) * 60);
    for (float &sample : noise) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        sample = static_cast<float>(static_cast<double>(state) / 4294967296.0 - 0.5);
    }
    InMemoryPcmSource source(noise, rate);
    SeabassGridAnalyser analyser;
    within(analyser.analyse(source).confidence, 0.0, 0.2, "noise: not sure of its grid");
}

// Sections of 64 beats that answer each other half a beat apart: the kick
// alone, then stabs between the beats alone, by turns. The track repeats
// 63.5 and 64.5 beats away as well as 64, and the tempo is still 126.
void sectionsThatAnswer()
{
    const int rate = 44100;
    const double period = 60.0 / 126.0;
    std::vector<float> audio(static_cast<std::size_t>((5 * 64 * period + 1.0) * rate), 0.0f);
    std::vector<Beat> truth;
    for (int k = 0; k < 5 * 64; ++k) {
        const double beat = 0.2 + k * period;
        truth.push_back({beat * 1000.0, k % 4 + 1});
        if ((k / 64) % 2 == 0) {
            addKick(audio, rate, beat);
        } else {
            addStab(audio, rate, beat + period / 2.0);
        }
    }
    InMemoryPcmSource source(audio, rate);
    SeabassGridAnalyser analyser;
    const GridAnalysis analysis = analyser.analyse(source);
    const BeatGridComparison c = compareBeatGrids(analysis.beats, truth);
    check(c.tempo == TempoRelation::Same, "sections that answer each other: the tempo of the track");
    within(static_cast<double>(analysis.beats.size()), 5 * 64 - 6.0, 5 * 64 + 6.0,
           "sections that answer each other: as many beats as the track has");
}

// One sample that is no number does not cost the track its grid.
void aSampleThatIsNoNumber()
{
    synth::SynthTrack track = synth::renderSynthTrack(*synth::synthPreset("house-124"));
    track.samples[track.samples.size() / 3] = std::numeric_limits<float>::quiet_NaN();
    track.samples[track.samples.size() / 2] = std::numeric_limits<float>::infinity();
    const GridAnalysis analysis = analysed(track);
    check(analysis.error.empty(), "a sample that is no number: not an error");
    const BeatGridComparison c = compareBeatGrids(analysis.beats, track.beats);
    check(c.tempo == TempoRelation::Same, "a sample that is no number: the grid all the same");
    within(c.offset.medianMs, -6.0, 0.0, "a sample that is no number: on the beat");
}

}  // namespace

// The one call a caller with decoded samples makes: the same grid as the
// analyser gives a source, the tempo as one number, the waveform, and the
// tempo range passed on.
void theOneCall()
{
    const synth::SynthTrack house = synth::renderSynthTrack(*synth::synthPreset("house-124"));
    const AudioAnalysis one = analyseAudio(house.samples, house.sampleRate);
    const GridAnalysis fromSource = analysed(house);
    check(one.grid.error.empty(), "the one call: no error");
    check(one.grid.beats.size() == fromSource.beats.size() && !one.grid.beats.empty(),
          "the one call: as many beats as the analyser gives a source");
    bool same = one.grid.beats.size() == fromSource.beats.size();
    for (std::size_t i = 0; same && i < one.grid.beats.size(); ++i) {
        same = one.grid.beats[i].timeMs == fromSource.beats[i].timeMs;
    }
    check(same, "the one call: the same beats to the last digit");
    within(one.bpm, 123.99, 124.01, "the one call: the tempo of a straight grid, not rounded");
    check(!one.grid.followsTempo, "the one call: a straight grid is said to be one");
    check(one.waveform.entries.size() == waveformEntryCount(house.samples.size(), house.sampleRate)
              && !one.waveform.entries.empty(),
          "the one call: a waveform of 150 entries a second");

    const synth::SynthTrack drummer = synth::renderSynthTrack(*synth::synthPreset("live-drummer-96"));
    const AudioAnalysis followed = analyseAudio(drummer.samples, drummer.sampleRate);
    check(followed.grid.followsTempo, "the one call: a drummer's grid is said to follow the tempo");
    within(followed.bpm, 90.0, 102.0, "the one call: a drummer's usual tempo");

    // The range goes through: a kick at 170 BPM is at 170 or 85 in the
    // default range and at a third or a quarter of it in 40 to 80.
    const int rate = 44100;
    std::vector<float> fast(static_cast<std::size_t>(rate) * 120, 0.0f);
    for (double at = 0.3; at < 118.0; at += 60.0 / 170.0) {
        addKick(fast, rate, at);
    }
    within(analyseAudio(fast, rate).bpm, 84.0, 171.0, "the one call: a kick at 170 in the default range");
    within(analyseAudio(fast, rate, TempoRange{40.0, 80.0}).bpm, 40.0, 80.0, "the one call: the same kick in 40 to 80");

    const AudioAnalysis nothing = analyseAudio({}, rate);
    check(!nothing.grid.error.empty() && nothing.grid.beats.empty() && nothing.bpm == 0.0 && nothing.waveform.entries.empty(),
          "the one call: no samples is an error, no grid, no tempo and no waveform");
}

int main()
{
    theOneCall();
    bassBetweenTheBeats();
    slowAndSparse();
    tempoRanges();
    shortLoop();
    wanderingDrummer(6.0, 0.35, true);
    wanderingDrummer(20.0, 0.8, false);
    stabsBetweenTheBeatsForASection();
    drummerWhoFallsBehindForAWhile();
    stabsOnAFifthOfTheTrack();
    movingInARange(45.0, 50.0, TempoRange{40.0, 80.0}, "a kick from 45 to 50 BPM in 40 to 80");
    movingInARange(205.0, 230.0, TempoRange{120.0, 240.0}, "a kick from 205 to 230 BPM in 120 to 240");
    heldTones();
    noiseIsUnsure();
    sectionsThatAnswer();
    aSampleThatIsNoNumber();
    spikeBetweenTheBeats(0.15, "a tick between the beats");
    spikeBetweenTheBeats(0.5, "a loud tick between the beats");
    nothingRegular();
    constantPreset("house-124", 124.0);
    constantPreset("house-124-48k", 124.0);
    // A tempo that is no whole number stays what it is.
    constantPreset("trance-137.98", 137.98);
    constantPreset("long-breakdown-126", 126.0);
    constantPreset("drumless-intro-128", 128.0);
    // A tempo that drifts, steps, ramps, alternates, and a drummer's.
    movingPreset("drift-122", 0.8, 0.95);
    movingPreset("step-120-126", 0.9, 0.95);
    movingPreset("ramp-100-125", 0.7, 0.8);
    movingPreset("alternating-127.5-128", 0.8, 0.95);
    movingPreset("live-drummer-96", 0.6, 0.95);
    noBeat();
    if (failures > 0) {
        std::cerr << failures << " check(s) FAILED\n";
        return 1;
    }
    std::cout << "seabass_grid_analyser_test: all checks passed\n";
    return 0;
}
