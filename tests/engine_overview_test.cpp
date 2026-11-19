// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The conversions of domain/engine_overview on inputs made up for the
// purpose, where the right answer follows from the definition: the
// resampling (overlap-weighted means over equal times, the source half
// an entry early), the fitted matrices applied and rounded and clipped,
// the final triple, the sample count rules. Whether the fitted numbers
// are any good against a real player is not this test's question; the
// calibration check in tools/ answers that on real tracks.

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "domain/engine_overview.hpp"

using namespace seabass::domain;

namespace
{

constexpr std::size_t N = EngineOverview::PointCount;

// A PWV6 body: `count` entries, each (b0, b1, b2) from the callback.
template<typename F>
std::string threeBand(std::size_t count, F entry)
{
    std::string bytes;
    for (std::size_t i = 0; i < count; ++i) {
        const BandTriple t = entry(i);
        bytes.push_back(static_cast<char>(t.low));
        bytes.push_back(static_cast<char>(t.mid));
        bytes.push_back(static_cast<char>(t.high));
    }
    return bytes;
}

std::string constant(std::size_t count, BandTriple value)
{
    return threeBand(count, [&](std::size_t) { return value; });
}

// The overview's mid byte for a PWV6 entry that has only b1, by the
// matrix in waveform_calibration.json (column M: b1 weight and the
// constant). Mid is the band where b1 counts most, so it is the one that
// shows the resampling.
std::uint8_t midFromB1(double b1)
{
    const double value = std::nearbyint(1.8067746248085388 * b1 + 6.74169331099033);
    return static_cast<std::uint8_t>(std::fmin(255.0, std::fmax(0.0, value)));
}

bool allPoints(const EngineOverview &overview, BandTriple expected)
{
    for (const auto &point : overview.points) {
        if (!(point == expected)) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main()
{
    // ---- the matrix on constant input --------------------------------
    //
    // A constant source resamples to the same constant everywhere, so
    // every point is the matrix applied to it. Worked from the JSON:
    // (10, 20, 30) gives 38.03, 47.85, 18.38; nothing gives 3.80, 6.74,
    // -2.00, the last clipped to 0; all 255 gives 1049.8 and 612.4,
    // clipped to 255, and 189.2.
    {
        const auto a = overviewFromThreeBandPreview(constant(1200, {10, 20, 30}), 300.0, 300.0);
        assert(a.points.size() == N);
        assert(allPoints(a, {38, 48, 18}));
        const auto zero = overviewFromThreeBandPreview(constant(1200, {0, 0, 0}), 300.0, 300.0);
        assert(allPoints(zero, {4, 7, 0}));
        const auto full = overviewFromThreeBandPreview(constant(1200, {255, 255, 255}), 300.0, 300.0);
        assert(allPoints(full, {255, 255, 189}));
        // The entry count is the data's: 600 or 5000 entries of the same
        // constant are the same overview.
        assert(allPoints(overviewFromThreeBandPreview(constant(600, {10, 20, 30}), 300.0, 300.0), {38, 48, 18}));
        assert(allPoints(overviewFromThreeBandPreview(constant(5000, {10, 20, 30}), 300.0, 300.0), {38, 48, 18}));
        std::cout << "case 1 (constant input: the matrix, rounding, clipping at 0 and 255) OK\n";
    }

    // ---- a step: the half-entry shift -------------------------------
    //
    // 1024 entries over the same time as the 1024 points: point j would
    // be entry j exactly, but the source is read half an entry late, so
    // point j is half entry j and half entry j + 1. With b1 stepping from
    // 0 to 100 at entry 512, point 511 is the mean, 50; and the last
    // point has only the half of entry 1023 that lies inside the source.
    {
        const auto step = overviewFromThreeBandPreview(
            threeBand(1024, [](std::size_t i) { return BandTriple{0, static_cast<std::uint8_t>(i < 512 ? 0 : 100), 0}; }),
            200.0, 200.0);
        assert(step.points.size() == N);
        assert(step.points[510].mid == midFromB1(0));
        assert(step.points[511].mid == midFromB1(50));
        assert(step.points[512].mid == midFromB1(100));
        assert(step.points[1023].mid == midFromB1(100));
        assert(midFromB1(50) == 97 && midFromB1(100) == 187);
        std::cout << "case 2 (a step: point 511 is half each side, the source half an entry late) OK\n";
    }

    // ---- a ramp -------------------------------------------------------
    {
        const auto ramp = overviewFromThreeBandPreview(
            threeBand(1024, [](std::size_t i) { return BandTriple{0, static_cast<std::uint8_t>(i / 8), 0}; }), 200.0,
            200.0);
        for (std::size_t j = 0; j + 1 < N; ++j) {
            const double mean = (static_cast<double>(j / 8) + static_cast<double>((j + 1) / 8)) / 2.0;
            assert(ramp.points[j].mid == midFromB1(mean));
        }
        assert(ramp.points[N - 1].mid == midFromB1(127));
        std::cout << "case 3 (a ramp: every point the mean of the two entries it straddles) OK\n";
    }

    // ---- durations: matched by time, never stretched -----------------
    {
        // The preview covers twice the player's length: the overview is
        // the preview's first half only. 2048 entries over 200 s against
        // 100 s, b1 20 then 100 from entry 1100 (the last point reaches into
        // entry 1024, half an entry late): all 20, none 100.
        const auto longer = overviewFromThreeBandPreview(
            threeBand(2048, [](std::size_t i) { return BandTriple{0, static_cast<std::uint8_t>(i < 1100 ? 20 : 100), 0}; }),
            200.0, 100.0);
        assert(longer.points.size() == N);
        for (const auto &point : longer.points) {
            assert(point.mid == midFromB1(20));
        }
        // The player's track twice the preview's: the second half of the
        // overview lies past the source's end and is what nothing gives.
        const auto shorter = overviewFromThreeBandPreview(constant(1024, {0, 100, 0}), 50.0, 100.0);
        assert(shorter.points.size() == N);
        for (std::size_t j = 0; j < N; ++j) {
            const BandTriple expected = j < 512 ? BandTriple{0, 187, 7} : BandTriple{4, 7, 0};
            assert(shorter.points[j] == expected);
        }
        std::cout << "case 4 (durations: times matched, the overhang empty, no stretching) OK\n";
    }

    // ---- the final triple ---------------------------------------------
    {
        const auto step = overviewFromThreeBandPreview(
            threeBand(1200,
                      [](std::size_t i) {
                          return i < 300 ? BandTriple{0, 100, 0} : i < 600 ? BandTriple{0, 0, 100} : BandTriple{0, 0, 0};
                      }),
            120.0, 120.0);
        // (0, 100, 0) gives (0, 187, 7); (0, 0, 100) gives (7, 3, 58);
        // nothing gives (4, 7, 0): the maximum of each band is 7, 187, 58,
        // a triple no single point has.
        assert(step.finalTriple == (BandTriple{7, 187, 58}));
        assert(step.finalTriple == perBandMaximum(step.points));
        assert(overviewFromThreeBandPreview(constant(1200, {10, 20, 30}), 300.0, 300.0).finalTriple
               == (BandTriple{38, 48, 18}));
        std::cout << "case 5 (the final triple is the per-band maximum) OK\n";
    }

    // ---- malformed input ------------------------------------------------
    {
        assert(overviewFromThreeBandPreview("", 100.0, 100.0).empty());
        assert(overviewFromThreeBandPreview(std::string(1201 * 3 - 1, '\x10'), 100.0, 100.0).empty());
        assert(overviewFromThreeBandPreview(constant(1200, {1, 2, 3}), 0.0, 100.0).empty());
        assert(overviewFromThreeBandPreview(constant(1200, {1, 2, 3}), 100.0, -1.0).empty());
        assert(overviewFromThreeBandPreview(constant(1200, {1, 2, 3}), std::nan(""), 100.0).empty());
        assert(overviewFromColourPreview(std::string(1200 * 6 + 3, '\x10'), 100.0, 100.0).empty());
        assert(secondWaveformFromOverview(EngineOverview{}).empty());
        std::cout << "case 6 (empty, ragged or timeless input is an empty overview) OK\n";
    }

    // ---- PWV4 ---------------------------------------------------------
    //
    // (100, 50, 80, 30, 120, 60) through the 6x3 matrix in the JSON:
    // 67.28, 123.41, 29.93.
    {
        std::string bytes;
        for (int i = 0; i < 1200; ++i) {
            bytes += std::string{100, 50, 80, 30, 120, 60};
        }
        const auto colour = overviewFromColourPreview(bytes, 300.0, 300.0);
        assert(colour.points.size() == N);
        assert(allPoints(colour, {67, 123, 30}));
        assert(colour.finalTriple == (BandTriple{67, 123, 30}));
        std::cout << "case 7 (the PWV4 fallback: six bytes an entry through its own matrix) OK\n";
    }

    // ---- the second waveform -----------------------------------------
    //
    // Through the 3x3 matrix of q6_rgb_fit.log: (100, 50, 20) gives
    // 62.907, 31.974, 31.689; (0, 0, 0) 2.397, 1.914, 2.169; (255, 255,
    // 255) 87.3, 236.0 and 374.5, clipped to 255.
    {
        EngineOverview overview;
        overview.points.assign(N, BandTriple{100, 50, 20});
        overview.points[0] = {0, 0, 0};
        overview.points[1] = {255, 255, 255};
        overview.finalTriple = perBandMaximum(overview.points);
        const auto second = secondWaveformFromOverview(overview);
        assert(second.points.size() == N);
        assert(second.points[0] == (BandTriple{2, 2, 2}));
        assert(second.points[1] == (BandTriple{87, 236, 255}));
        assert(second.points[2] == (BandTriple{63, 32, 32}));
        assert(second.finalTriple == (BandTriple{87, 236, 255}));
        std::cout << "case 8 (the .rgb waveform from the overview, its final triple the maximum) OK\n";
    }

    // ---- sample counts --------------------------------------------------
    {
        // Every whole number of MP3 frames, through rekordbox's entry
        // count (samples / 294, rounded up) and back. An entry is narrower
        // than a frame, so there is never a second candidate to choose.
        for (std::uint64_t frames = 1; frames < 40000; frames += 7) {
            const std::uint64_t samples = frames * 1152;
            const auto entries = static_cast<std::uint32_t>((samples + 293) / 294);
            assert(engineSampleCount(entries, 44100.0, true) == samples);
            // The same rule at 48 kHz, 320 samples an entry: what the
            // rule implies there, not a measurement.
            const auto entries48 = static_cast<std::uint32_t>((samples + 319) / 320);
            assert(engineSampleCount(entries48, 48000.0, true) == samples);
        }
        // Any entry count at all gives whole frames.
        for (std::uint32_t entries = 1; entries < 20000; ++entries) {
            assert(engineSampleCount(entries, 44100.0, true) % 1152 == 0);
        }
        // Four real MP3s the player had counted.
        assert(engineSampleCount(65688, 44100.0, true) == 19312128);
        assert(engineSampleCount(63925, 44100.0, true) == 18793728);
        assert(engineSampleCount(28616, 44100.0, true) == 8413056);
        assert(engineSampleCount(44368, 44100.0, true) == 13044096);
        // Anything else: the middle of the last entry. 100 entries is
        // 29400 samples, less half an entry; and a real M4A the player
        // counted 13320182 samples, 71 (1.6 ms) from the estimate.
        assert(engineSampleCount(100, 44100.0, false) == 29253);
        const auto m4a = static_cast<double>(engineSampleCount(45307, 44100.0, false));
        assert(std::fabs(m4a - 13320182.0) < 0.003 * 44100.0);
        assert(engineSampleCount(0, 44100.0, true) == 0);
        assert(engineSampleCount(100, 0.0, false) == 0);
        std::cout << "case 9 (sample counts: whole MP3 frames in the last entry, else its middle) OK\n";
    }

    // ---- loudness -------------------------------------------------------
    {
        EngineOverview silent;
        silent.points.assign(N, BandTriple{0, 0, 0});
        const auto quiet = loudnessFromOverview(silent);
        assert(std::fabs(quiet[0] - 0.4180991910089868) < 1e-12);
        assert(std::fabs(quiet[1] - 0.20371712901360284) < 1e-12);
        assert(std::fabs(quiet[2] - 0.08844842823637834) < 1e-12);
        EngineOverview half;
        half.points.assign(N / 2, BandTriple{255, 255, 255});
        half.points.resize(N, BandTriple{0, 0, 0});
        const auto mixed = loudnessFromOverview(half);
        assert(std::fabs(mixed[0] - (0.60555172422179 * 0.5 + 0.4180991910089868)) < 1e-12);
        assert(std::fabs(mixed[2] - (2.3108329350136936 * 0.5 + 0.08844842823637834)) < 1e-12);
        const auto none = loudnessFromOverview(EngineOverview{});
        assert(none[0] == 0.0 && none[1] == 0.0 && none[2] == 0.0);
        std::cout << "case 10 (loudness: a straight line per band through the band's mean) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
