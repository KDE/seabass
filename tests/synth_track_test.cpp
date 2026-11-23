// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The synthetic tracks (tools/synth_track) against their own description:
//
//   - one seed gives one track, sample for sample, within this process,
//     and another seed another track on the same grid;
//   - every preset's true grid is the tempo programme its description
//     states, worked out here again from the numbers alone;
//   - the kicks are in the audio where the grid says: in the kick band,
//     each kick's onset within 1 ms of the beat it is on (of its jittered
//     time for the live drummer, whose kicks scatter around the true beat
//     with the deviation the preset states);
//   - what the arrangement leaves out is not there.
//
// No bytes are compared with stored ones: the samples go through sin,
// exp and tanh, which may differ in the last bit between C libraries.
// The generator's integers do not, so its first draws are pinned.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

#include "synth_track/synth_track.hpp"

using namespace seabass;
using synth::Instrument;

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

synth::SynthTrackSpec preset(const std::string &name)
{
    const auto spec = synth::synthPreset(name);
    if (!spec) {
        std::cerr << "FAILED: no preset " << name << "\n";
        ++failures;
        return {};
    }
    return *spec;
}

void randomGenerator()
{
    // Integer arithmetic only, so these are the same on every platform.
    synth::Random random(42);
    const std::uint64_t first = random.next();
    const std::uint64_t second = random.next();
    check(first == 1546998764402558742ULL, "random: first draw pinned, got " + std::to_string(first));
    synth::Random again(42);
    check(again.next() == first && again.next() == second, "random: same seed, same draws");
    synth::Random other(43);
    check(other.next() != first, "random: another seed, another draw");

    synth::Random stats(7);
    double sum = 0.0;
    double sumSquares = 0.0;
    double normalSum = 0.0;
    double normalSquares = 0.0;
    double low = 1.0;
    double high = 0.0;
    constexpr int N = 200000;
    for (int i = 0; i < N; ++i) {
        const double u = stats.uniform();
        low = std::min(low, u);
        high = std::max(high, u);
        sum += u;
        sumSquares += u * u;
        const double g = stats.normal();
        normalSum += g;
        normalSquares += g * g;
    }
    const double mean = sum / N;
    const double variance = sumSquares / N - mean * mean;
    check(low >= 0.0 && high < 1.0, "uniform: in [0, 1)");
    check(std::abs(mean - 0.5) < 0.005, "uniform: mean " + std::to_string(mean));
    check(std::abs(variance - 1.0 / 12.0) < 0.002, "uniform: variance " + std::to_string(variance));
    const double normalMean = normalSum / N;
    const double normalSd = std::sqrt(normalSquares / N - normalMean * normalMean);
    check(std::abs(normalMean) < 0.01, "normal: mean " + std::to_string(normalMean));
    check(std::abs(normalSd - 1.0) < 0.01, "normal: deviation " + std::to_string(normalSd));
}

// What each preset's description says its grid is, beat interval by beat
// interval, from the numbers in the description and nothing else.
struct Expected
{
    const char *name;
    double firstMs;
    int beats;
    std::function<double(int)> bpm;
};

std::vector<Expected> expectations()
{
    constexpr double Pi = std::numbers::pi;
    return {
        {"house-124", 0.0, 224, [](int) { return 124.0; }},
        {"trance-137.98", 0.0, 224, [](int) { return 137.98; }},
        {"drift-122", 0.0, 224, [](int k) { return 122.0 + 0.5 * std::sin(2.0 * Pi * k / 96.0); }},
        {"ramp-100-125", 0.0, 224,
         [](int k) { return k < 16 ? 100.0 : k < 144 ? 100.0 + 25.0 * (k - 16) / 128.0 : 125.0; }},
        {"step-120-126", 0.0, 224, [](int k) { return k < 64 ? 120.0 : 126.0; }},
        {"alternating-127.5-128", 0.0, 224, [](int k) { return (k / 32) % 2 == 0 ? 127.5 : 128.0; }},
        {"live-drummer-96", 0.0, 176, [](int k) { return 96.0 + 0.8 * std::sin(2.0 * Pi * k / 128.0); }},
        {"breakbeat-swing-132", 0.0, 224, [](int) { return 132.0; }},
        {"long-breakdown-126", 0.0, 320, [](int) { return 126.0; }},
        {"drumless-intro-128", 2345.0, 192, [](int) { return 128.0; }},
        {"house-124-48k", 0.0, 224, [](int) { return 124.0; }},
        {"halftime-174", 0.0, 224, [](int) { return 174.0; }},
    };
}

void grids()
{
    const auto names = synth::synthPresetNames();
    const auto expected = expectations();
    check(names.size() == expected.size(), "every preset has an expectation here");
    for (const Expected &e : expected) {
        check(std::find(names.begin(), names.end(), e.name) != names.end(), std::string("preset listed: ") + e.name);
        const auto beats = synth::synthGrid(preset(e.name));
        check(static_cast<int>(beats.size()) == e.beats,
              std::string(e.name) + ": " + std::to_string(beats.size()) + " beats");
        double time = e.firstMs;
        double worst = 0.0;
        bool barsRight = true;
        for (std::size_t k = 0; k < beats.size(); ++k) {
            worst = std::max(worst, std::abs(beats[k].timeMs - time));
            barsRight = barsRight && beats[k].beatInBar == static_cast<int>(k % 4) + 1;
            time += 60000.0 / e.bpm(static_cast<int>(k));
        }
        check(worst < 1e-6, std::string(e.name) + ": grid off its programme by " + std::to_string(worst) + " ms");
        check(barsRight, std::string(e.name) + ": bars count 1 to 4 from the first beat");
    }
}

// The kick band without delay: a low-pass at 300 Hz run forwards and then
// backwards, so whatever it smears it smears to both sides alike. 300 Hz
// passes the kick's 155 Hz start whole; a lower corner takes half of it
// away and moves the half-peak point onto the next crest, 2.6 ms late.
std::vector<double> kickBand(const std::vector<float> &samples, int sampleRate)
{
    const double w = 2.0 * std::numbers::pi * 300.0 / sampleRate;
    const double alpha = std::sin(w) / (2.0 * 0.7071);
    const double a0 = 1.0 + alpha;
    const double b0 = (1.0 - std::cos(w)) / 2.0 / a0;
    const double b1 = (1.0 - std::cos(w)) / a0;
    const double a1 = -2.0 * std::cos(w) / a0;
    const double a2 = (1.0 - alpha) / a0;
    const auto pass = [&](std::vector<double> &x) {
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        for (double &v : x) {
            const double y = b0 * v + b1 * x1 + b0 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = v;
            y2 = y1;
            y1 = y;
            v = y;
        }
    };
    std::vector<double> band(samples.begin(), samples.end());
    pass(band);
    std::reverse(band.begin(), band.end());
    pass(band);
    std::reverse(band.begin(), band.end());
    return band;
}

// Where the kick band first reaches half its peak within `windowMs` of
// `aroundMs`, in ms; a hit that starts at full strength reaches it at
// its start. -1 for an empty window.
double onsetNear(const std::vector<double> &band, int sampleRate, double aroundMs, double windowMs)
{
    const auto toIndex = [&](double ms) {
        return static_cast<long>(std::llround(ms * sampleRate / 1000.0));
    };
    const long from = std::max(0L, toIndex(aroundMs - windowMs));
    const long to = std::min(static_cast<long>(band.size()) - 1, toIndex(aroundMs + windowMs));
    double peak = 0.0;
    for (long i = from; i <= to; ++i) {
        peak = std::max(peak, std::abs(band[static_cast<std::size_t>(i)]));
    }
    if (peak <= 0.0) {
        return -1.0;
    }
    for (long i = from; i <= to; ++i) {
        if (std::abs(band[static_cast<std::size_t>(i)]) >= 0.5 * peak) {
            return i * 1000.0 / sampleRate;
        }
    }
    return -1.0;
}

struct Rendered
{
    synth::SynthTrackSpec spec;
    synth::SynthTrack track;
};

void basics(const Rendered &r)
{
    const std::string name = r.spec.name;
    const auto &samples = r.track.samples;
    check(r.track.sampleRate == r.spec.sampleRate, name + ": rate");
    bool finite = true;
    double peak = 0.0;
    double energy = 0.0;
    for (const float s : samples) {
        finite = finite && std::isfinite(s);
        peak = std::max(peak, static_cast<double>(std::abs(s)));
        energy += static_cast<double>(s) * s;
    }
    check(finite, name + ": every sample a number");
    // The limiter ends at -1 dBFS.
    check(peak <= 0.8913 && peak > 0.85, name + ": peak " + std::to_string(peak));
    const double rms = std::sqrt(energy / static_cast<double>(samples.size()));
    check(rms > 0.05 && rms < 0.5, name + ": loudness " + std::to_string(rms));
    const double endMs = synth::synthGrid(r.spec).back().timeMs;
    check(samples.size() * 1000.0 / r.track.sampleRate > endMs + r.spec.tailMs - 1.0, name + ": length covers the tail");
    check(r.track.beats.size() == synth::synthGrid(r.spec).size(), name + ": the track carries its grid");
}

void kicksOnTheGrid(const Rendered &r)
{
    const std::string name = r.spec.name;
    const auto band = kickBand(r.track.samples, r.track.sampleRate);
    int onBeat = 0;
    double worstToHit = 0.0;
    double worstToBeat = 0.0;
    std::vector<double> deviations;
    for (const synth::SynthHit &hit : r.track.hits) {
        if (hit.instrument != Instrument::Kick) {
            continue;
        }
        // A bass note that starts near a kick but not with it (a drummer
        // off the beat the bass player is on) is in the kick band too,
        // and as loud: such a kick is not on its own there to be timed.
        const bool bassNear = std::any_of(r.track.hits.begin(), r.track.hits.end(), [&](const synth::SynthHit &other) {
            return other.instrument == Instrument::Bass && std::abs(other.timeMs - hit.timeMs) < 30.0
                && std::abs(other.timeMs - hit.timeMs) > 0.001;
        });
        if (bassNear) {
            continue;
        }
        // Found in the audio around the hit's own time...
        const double onset = onsetNear(band, r.track.sampleRate, hit.timeMs, 25.0);
        worstToHit = std::max(worstToHit, std::abs(onset - hit.timeMs));
        if (hit.beatIndex < 0) {
            continue;
        }
        ++onBeat;
        // ...and around the true beat, not knowing the hit's time.
        const double beatMs = r.track.beats[static_cast<std::size_t>(hit.beatIndex)].timeMs;
        const double fromBeat = onsetNear(band, r.track.sampleRate, beatMs, 40.0) - beatMs;
        deviations.push_back(fromBeat);
        if (r.spec.jitterMs == 0.0) {
            worstToBeat = std::max(worstToBeat, std::abs(fromBeat));
            check(hit.timeMs == beatMs, name + ": a machine's kick is on the beat exactly");
        }
    }
    check(onBeat >= 40, name + ": kicks on the beat: " + std::to_string(onBeat));
    check(worstToHit <= 1.0, name + ": every kick in the audio at its time, worst " + std::to_string(worstToHit) + " ms");
    if (r.spec.jitterMs == 0.0) {
        check(worstToBeat <= 1.0, name + ": every kick within 1 ms of its beat, worst " + std::to_string(worstToBeat));
        return;
    }
    // The live drummer: the kicks scatter around the true beat by the
    // stated deviation and are not early or late on the whole.
    double sum = 0.0;
    double squares = 0.0;
    for (const double d : deviations) {
        sum += d;
        squares += d * d;
    }
    const double mean = sum / static_cast<double>(deviations.size());
    const double sd = std::sqrt(squares / static_cast<double>(deviations.size()) - mean * mean);
    check(std::abs(mean) < 2.0, name + ": kicks centred on the beat, mean " + std::to_string(mean));
    check(deviations.size() >= 40, name + ": kicks measured alone: " + std::to_string(deviations.size()));
    // A quarter either way: about two standard errors of a deviation
    // estimated from 48 kicks.
    check(sd > 0.75 * r.spec.jitterMs && sd < 1.25 * r.spec.jitterMs,
          name + ": kicks scatter by " + std::to_string(sd) + " ms, stated " + std::to_string(r.spec.jitterMs));
}

void arrangement(const Rendered &r)
{
    const std::string name = r.spec.name;
    // Bars of each section, to ask what plays where.
    std::vector<const synth::Section *> sectionOfBar;
    for (const synth::Section &section : r.spec.sections) {
        for (int i = 0; i < section.bars; ++i) {
            sectionOfBar.push_back(&section);
        }
    }
    int misplaced = 0;
    for (const synth::SynthHit &hit : r.track.hits) {
        if (hit.instrument != Instrument::Kick) {
            continue;
        }
        // The bar the hit is in: the last beat at or before it, less
        // the jitter's reach.
        const auto after = std::upper_bound(r.track.beats.begin(), r.track.beats.end(), hit.timeMs + 30.0,
                                            [](double t, const domain::Beat &b) { return t < b.timeMs; });
        const auto beat = static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, after - r.track.beats.begin() - 1));
        const std::size_t bar = std::min(beat / 4, sectionOfBar.size() - 1);
        if (!sectionOfBar[bar]->kick && !(bar > 0 && sectionOfBar[bar - 1]->kick)) {
            ++misplaced;
        }
    }
    check(misplaced == 0, name + ": kicks only in sections with a kick, " + std::to_string(misplaced) + " out of place");

    // Nothing before the first beat.
    const auto before = static_cast<std::size_t>(r.spec.firstBeatMs * r.track.sampleRate / 1000.0);
    double loudest = 0.0;
    for (std::size_t i = 0; i < before && i < r.track.samples.size(); ++i) {
        loudest = std::max(loudest, static_cast<double>(std::abs(r.track.samples[i])));
    }
    check(loudest == 0.0, name + ": silent before the first beat");
}

void determinism()
{
    const auto spec = preset("live-drummer-96");
    const auto a = synth::renderSynthTrack(spec);
    const auto b = synth::renderSynthTrack(spec);
    check(a.samples == b.samples, "same seed: same samples");
    check(a.hits.size() == b.hits.size(), "same seed: same hits");
    bool sameHits = a.hits.size() == b.hits.size();
    for (std::size_t i = 0; sameHits && i < a.hits.size(); ++i) {
        sameHits = a.hits[i].timeMs == b.hits[i].timeMs && a.hits[i].instrument == b.hits[i].instrument;
    }
    check(sameHits, "same seed: hits at the same times");

    auto reseeded = spec;
    reseeded.seed = spec.seed + 1;
    const auto c = synth::renderSynthTrack(reseeded);
    check(c.samples.size() == a.samples.size(), "another seed: same length");
    check(c.samples != a.samples, "another seed: other samples");
    bool sameGrid = c.beats.size() == a.beats.size();
    for (std::size_t i = 0; sameGrid && i < a.beats.size(); ++i) {
        sameGrid = c.beats[i].timeMs == a.beats[i].timeMs;
    }
    check(sameGrid, "another seed: the same true grid");
}

void swing()
{
    const auto spec = preset("breakbeat-swing-132");
    const auto track = synth::renderSynthTrack(spec);
    // Each off sixteenth sits 58 percent of the way through its eighth.
    int checked = 0;
    double worst = 0.0;
    for (const synth::SynthHit &hit : track.hits) {
        if (hit.instrument != Instrument::Hat && hit.instrument != Instrument::OpenHat) {
            continue;
        }
        const auto after = std::upper_bound(track.beats.begin(), track.beats.end(), hit.timeMs,
                                            [](double t, const domain::Beat &b) { return t < b.timeMs; });
        const auto k = static_cast<std::size_t>(after - track.beats.begin() - 1);
        if (k + 1 >= track.beats.size()) {
            continue;
        }
        const double period = track.beats[k + 1].timeMs - track.beats[k].timeMs;
        const double fraction = (hit.timeMs - track.beats[k].timeMs) / period;
        const double inEighth = std::fmod(fraction, 0.5) / 0.5;
        // On the eighth itself (either end, as the arithmetic rounds) is
        // not swung.
        if (inEighth > 1e-6 && inEighth < 1.0 - 1e-6) {
            worst = std::max(worst, std::abs(inEighth - 0.58));
            ++checked;
        }
    }
    check(checked > 100, "swing: off sixteenths found");
    check(worst < 1e-9, "swing: every off sixteenth at 58 percent of its eighth, worst " + std::to_string(worst));
}

}  // namespace

int main()
{
    randomGenerator();
    grids();
    determinism();
    swing();
    for (const std::string &name : synth::synthPresetNames()) {
        Rendered r{preset(name), {}};
        r.track = synth::renderSynthTrack(r.spec);
        basics(r);
        kicksOnTheGrid(r);
        arrangement(r);
    }
    if (failures > 0) {
        std::cerr << failures << " check(s) FAILED\n";
        return 1;
    }
    std::cout << "synth_track_test: all checks passed\n";
    return 0;
}
