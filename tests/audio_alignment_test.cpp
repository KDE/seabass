// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The arithmetic that lines two copies of a song up (domain/
// audio_alignment), against signals made here: noise with a known delay,
// a known different tail, a section cut out, another signal entirely.
// No decoder is involved; alignment_probe_test runs the same rule on
// files the real one decodes.

#include <cassert>
#include <cmath>
#include <complex>
#include <iostream>
#include <numbers>
#include <random>
#include <vector>

#include "domain/audio_alignment.hpp"

using namespace seabass::domain::alignment;

namespace
{

std::vector<float> noise(size_t samples, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<float> dist(0.0f, 0.3f);
    std::vector<float> out(samples);
    for (auto &value : out) {
        value = dist(rng);
    }
    return out;
}

std::vector<float> concat(std::vector<float> a, const std::vector<float> &b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

std::vector<float> silence(size_t samples)
{
    return std::vector<float>(samples, 0.0f);
}

bool near(double a, double b, double tolerance)
{
    return std::abs(a - b) <= tolerance;
}

}  // namespace

int main()
{
    // The transform against a plain DFT, and back again.
    {
        std::vector<std::complex<double>> data(16);
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = {std::sin(0.7 * static_cast<double>(i)) + 0.1 * static_cast<double>(i), std::cos(1.3 * i)};
        }
        const auto original = data;
        fft(data, false);
        for (size_t k = 0; k < data.size(); ++k) {
            std::complex<double> expected;
            for (size_t n = 0; n < original.size(); ++n) {
                expected += original[n] * std::polar(1.0, -2.0 * std::numbers::pi * static_cast<double>(k * n) / 16.0);
            }
            assert(std::abs(data[k] - expected) < 1e-9 && "the forward transform is a DFT");
        }
        fft(data, true);
        for (size_t n = 0; n < data.size(); ++n) {
            assert(std::abs(data[n] - original[n]) < 1e-12 && "the inverse undoes it");
        }
    }

    // Resampling keeps time exact. At 44.1 kHz to 22.05 kHz an impulse at
    // input sample 2m stays at output sample m.
    {
        Resampler r(44100.0, 22050.0);
        for (int k = 0; k < 2000; ++k) {
            r.push(k == 1234 ? 1.0f : 0.0f);
        }
        const auto out = r.finish();
        size_t peak = 0;
        for (size_t n = 0; n < out.size(); ++n) {
            if (out[n] > out[peak]) {
                peak = n;
            }
        }
        assert(peak == 617 && "an impulse stays where it was");
        assert(out.size() >= 999 && out.size() <= 1001);
    }
    // At a ratio that is no integer (48 kHz) every output sample still
    // stands for its own instant: a ramp whose value is its time comes out
    // as a ramp whose value is the output's time. Half an output sample
    // late would be 23 us off; this holds it to 2 us.
    {
        Resampler r(48000.0, 22050.0);
        for (int k = 0; k < 48000; ++k) {
            r.push(static_cast<float>(k / 48000.0));
        }
        const auto out = r.finish();
        assert(out.size() >= 22049 && out.size() <= 22051);
        for (size_t n = 2; n + 2 < out.size(); ++n) {
            assert(near(out[n], static_cast<double>(n) / 22050.0, 2e-6) && "the 48 kHz output is on time");
        }
    }
    // Upsampling (a 16 kHz file) too.
    {
        Resampler r(16000.0, 22050.0);
        for (int k = 0; k < 16000; ++k) {
            r.push(static_cast<float>(k / 16000.0));
        }
        const auto out = r.finish();
        for (size_t n = 3; n + 3 < out.size(); ++n) {
            assert(near(out[n], static_cast<double>(n) / 22050.0, 2e-5) && "the 16 kHz output is on time");
        }
    }

    // One window's peak: the lag is where a sound sits in b minus where
    // it sits in a, either way round.
    {
        const auto a = noise(5000, 1);
        auto peak = correlationPeak(a, concat(silence(137), a), 500);
        assert(peak && peak->lag == 137 && peak->normalised > 0.99 && "b starts 137 samples later");
        const std::vector<float> trimmed(a.begin() + 200, a.end());
        peak = correlationPeak(a, trimmed, 500);
        assert(peak && peak->lag == -200 && peak->normalised > 0.99 && "b starts 200 samples earlier");
        peak = correlationPeak(a, noise(5000, 2), 500);
        assert(peak && peak->normalised < 0.2 && "another signal correlates with nothing");
        assert(!correlationPeak(a, a, 6000) && "less overlap than asked for is no answer");
        assert(!correlationPeak(silence(3000), a, 500) && "silence has nothing to correlate");
    }

    // The rule.
    {
        auto clean = cleanShiftMs({{10.0, 0.9}, {10.5, 0.8}, {11.0, 0.95}});
        assert(clean && near(*clean, 10.5, 1e-9) && "the median of three that agree");
        clean = cleanShiftMs({{10.0, 0.9}, {12.0, 0.8}, {11.0, 0.95}});
        assert(clean && near(*clean, 11.0, 1e-9) && "2 ms apart still agrees");
        assert(!cleanShiftMs({{10.0, 0.9}, {12.01, 0.8}, {11.0, 0.95}}) && "just over 2 ms apart does not");
        assert(!cleanShiftMs({{10.0, 0.9}, {10.0, 0.49}, {10.0, 0.95}}) && "one weak window is no answer");
        clean = cleanShiftMs({{10.0, 0.5}, {10.0, 0.5}, {10.0, 0.5}});
        assert(clean && "0.5 is enough");
        assert(!cleanShiftMs({}) && "no windows, no answer");
    }

    // Whole files, at a low analysis rate so the test stays quick: the
    // windows are as long in seconds as ever, only fewer samples. 100 s,
    // so start, middle and end are three different stretches.
    {
        constexpr double rate = 2000.0;
        const auto a = noise(200000, 7);
        const double sampleMs = 1000.0 / rate;

        auto shift = measureShiftMs(a, a, rate);
        assert(shift && near(*shift, 0.0, 1e-9) && "a file lines up with itself");

        // Leading silence: the music in b starts 431 samples later.
        shift = measureShiftMs(a, concat(silence(431), a), rate);
        assert(shift && near(*shift, 431 * sampleMs, 1e-6) && "prepended silence is the shift");
        shift = measureShiftMs(concat(silence(431), a), a, rate);
        assert(shift && near(*shift, -431 * sampleMs, 1e-6) && "and the other way round");

        // A different tail moves nothing: the end window is counted from
        // each file's own end and turned back into a shift from the start.
        shift = measureShiftMs(a, concat(a, noise(14000, 8)), rate);
        assert(shift && near(*shift, 0.0, 1e-6) && "a longer tail is not a shift");
        shift = measureShiftMs(a, concat(concat(silence(300), a), noise(9000, 9)), rate);
        assert(shift && near(*shift, 300 * sampleMs, 1e-6) && "lead-in and tail together");

        // Not the same music.
        assert(!measureShiftMs(a, noise(200000, 11), rate) && "another signal does not line up");

        // A section cut from the middle of b: the start agrees, the end
        // is 150 ms early, and no one shift is right for every cue.
        std::vector<float> cut(a.begin(), a.begin() + 100000);
        cut.insert(cut.end(), a.begin() + 100300, a.end());
        assert(!measureShiftMs(a, cut, rate) && "start and end disagree");

        // Shorter than a window: the windows are the whole files.
        const std::vector<float> shortA(a.begin(), a.begin() + 20000);
        shift = measureShiftMs(shortA, concat(silence(50), shortA), rate);
        assert(shift && near(*shift, 50 * sampleMs, 1e-6) && "a 10 s file still lines up");

        // Under a second: too little to say.
        const std::vector<float> tiny(a.begin(), a.begin() + 1500);
        assert(!measureShiftMs(tiny, tiny, rate) && "under a second is unknown");
    }

    std::cout << "audio_alignment_test: all assertions passed\n";
    return 0;
}
