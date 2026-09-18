// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// AudioLevelMeter on signals whose answer is known: a pure tone belongs
// to one band, silence to none, and a kick every half second is a beat
// every half second.

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "domain/audio_levels.hpp"

using namespace seabass::domain;

namespace
{
int failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

constexpr int kRate = 44100;
constexpr std::size_t kBlock = 1024;  // about 23 ms, what a player hands over
constexpr double kPi = 3.14159265358979323846;

// `seconds` of a sine, fed block by block. `gate(t)` scales it over time.
template <typename Gate>
AudioLevels feedTone(AudioLevelMeter &meter, double hz, double seconds, Gate gate, int *beats = nullptr)
{
    AudioLevels levels;
    std::vector<float> block(kBlock);
    const auto total = static_cast<std::size_t>(seconds * kRate);
    for (std::size_t start = 0; start < total; start += kBlock) {
        for (std::size_t i = 0; i < kBlock; ++i) {
            const double t = static_cast<double>(start + i) / kRate;
            block[i] = static_cast<float>(0.8 * gate(t) * std::sin(2.0 * kPi * hz * t));
        }
        levels = meter.feed(block.data(), kBlock, kRate);
        if (beats != nullptr && levels.beat) {
            ++*beats;
        }
    }
    return levels;
}

double always(double) { return 1.0; }
}  // namespace

int main()
{
    // A bass tone is low band and nothing else.
    {
        AudioLevelMeter meter;
        const AudioLevels levels = feedTone(meter, 60.0, 1.0, always);
        check(levels.low > 0.8, "a 60 Hz tone fills the low band");
        check(levels.mid < 0.45 && levels.mid < levels.low / 2.0, "and leaves the mid band far behind");
        check(levels.high < 0.1, "and the high band empty");
    }
    // A high tone is the high band's.
    {
        AudioLevelMeter meter;
        const AudioLevels levels = feedTone(meter, 9000.0, 1.0, always);
        check(levels.high > 0.8, "a 9 kHz tone fills the high band");
        check(levels.low < 0.05, "and leaves the low band empty");
    }
    // A mid tone is the mid band's more than anyone's.
    {
        AudioLevelMeter meter;
        const AudioLevels levels = feedTone(meter, 800.0, 1.0, always);
        check(levels.mid > 0.8, "an 800 Hz tone fills the mid band");
        check(levels.mid > levels.low, "more than the low one");
    }
    // Levels fall back once the sound has gone, and not at once.
    {
        AudioLevelMeter meter;
        feedTone(meter, 60.0, 0.5, always);
        const AudioLevels soon = feedTone(meter, 60.0, 0.05, [](double) { return 0.0; });
        check(soon.low > 0.3, "50 ms after the sound stops the level is still falling");
        const AudioLevels later = feedTone(meter, 60.0, 1.5, [](double) { return 0.0; });
        check(later.low < 0.01, "and after a second and a half it is gone");
    }
    // A kick every half second is a beat every half second: eight of them
    // in 3.9 s (at 4.0 the ninth starts).
    {
        AudioLevelMeter meter;
        int beats = 0;
        feedTone(meter, 60.0, 3.9, [](double t) { return std::fmod(t, 0.5) < 0.08 ? 1.0 : 0.0; }, &beats);
        check(beats == 8, "a kick every half second is a beat every half second");
        if (beats != 8) {
            std::cerr << "  counted " << beats << "\n";
        }
    }
    // A bass note held is one beat, when it starts -- not one per block.
    {
        AudioLevelMeter meter;
        int beats = 0;
        feedTone(meter, 60.0, 3.0, always, &beats);
        check(beats == 1, "a held bass note is a single beat");
    }
    // Hi-hats alone are no beat at all.
    {
        AudioLevelMeter meter;
        int beats = 0;
        feedTone(meter, 9000.0, 3.0, [](double t) { return std::fmod(t, 0.25) < 0.03 ? 1.0 : 0.0; }, &beats);
        check(beats == 0, "the beat is in the low band; hi-hats are not it");
    }
    // Levels are relative to the track's own loudness: a quiet track
    // moves as much as a loud one.
    {
        AudioLevelMeter loud;
        AudioLevelMeter quiet;
        const AudioLevels loudLevels = feedTone(loud, 60.0, 1.0, always);
        const AudioLevels quietLevels = feedTone(quiet, 60.0, 1.0, [](double) { return 0.1; });
        check(std::abs(loudLevels.low - quietLevels.low) < 0.02, "a tenth of the amplitude reads the same");
    }
    // But near-silence is not amplified into a show.
    {
        AudioLevelMeter meter;
        const AudioLevels levels = feedTone(meter, 60.0, 1.0, [](double) { return 0.002; });
        check(levels.low < 0.15, "a whisper of hum stays a whisper");
    }
    // Interleaved 16-bit stereo, as a player really hands it over, reads
    // as the same tone fed as mono floats does; and the channels are
    // averaged, so the same tone in opposite phase on the two is silence.
    {
        std::vector<std::int16_t> stereo(kBlock * 2);
        std::vector<std::int16_t> opposed(kBlock * 2);
        std::vector<float> mono(kBlock);
        AudioLevelMeter fromStereo;
        AudioLevelMeter fromOpposed;
        AudioLevelMeter fromMono;
        AudioLevels stereoLevels;
        AudioLevels opposedLevels;
        AudioLevels monoLevels;
        for (int blockIndex = 0; blockIndex < 40; ++blockIndex) {
            for (std::size_t i = 0; i < kBlock; ++i) {
                const double t = static_cast<double>(blockIndex * kBlock + i) / kRate;
                const double x = 0.5 * std::sin(2.0 * kPi * 60.0 * t) * (blockIndex < 20 ? 1.0 : 0.3);
                const auto sample = static_cast<std::int16_t>(x * 32767.0);
                stereo[i * 2] = sample;
                stereo[i * 2 + 1] = sample;
                opposed[i * 2] = sample;
                opposed[i * 2 + 1] = static_cast<std::int16_t>(-sample);
                mono[i] = static_cast<float>(x);
            }
            stereoLevels = fromStereo.feed(stereo.data(), kBlock, 2, SampleFormat::Int16, kRate);
            opposedLevels = fromOpposed.feed(opposed.data(), kBlock, 2, SampleFormat::Int16, kRate);
            monoLevels = fromMono.feed(mono.data(), kBlock, kRate);
        }
        check(stereoLevels.low > 0.2 && stereoLevels.low < 0.5, "a tone dropped to 0.3 of what it was reads about 0.36");
        check(std::abs(stereoLevels.low - monoLevels.low) < 0.01, "16-bit stereo reads as the same floats in mono");
        check(opposedLevels.low < 0.001, "and opposite phase on the two channels averages to silence");
    }
    // Nothing to feed is not an error and not a beat.
    {
        AudioLevelMeter meter;
        const AudioLevels levels = meter.feed(nullptr, 0, 2, SampleFormat::Int16, kRate);
        check(levels.low == 0.0 && !levels.beat, "an empty block changes nothing");
    }

    if (failures == 0) {
        std::cout << "audio_levels_test: all cases passed\n";
    }
    return failures == 0 ? 0 : 1;
}
