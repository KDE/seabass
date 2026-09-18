// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace seabass::domain
{

// How loud the music is right now in three bands, each 0..1 of the
// track's own recent loudest, and whether a beat just landed. What a display moves to while a track plays.
struct AudioLevels
{
    double low = 0.0;
    double mid = 0.0;
    double high = 0.0;
    bool beat = false;
};

enum class SampleFormat { UInt8, Int16, Int32, Float };

// Turns the player's decoded audio, block by block as it is played, into
// AudioLevels. Deliberately cheap -- four second-order filters and three
// running sums, no FFT: it runs for every block of every track played,
// and a display needs "the bass just hit", not a spectrum.
//
// The stored waveform cannot do this. A preview has 400 columns to the
// track, one a second or so; the beat is in the audio and nowhere else
// the player can reach.
class AudioLevelMeter
{
public:
    // One block of interleaved samples as the player hands them over.
    // Channels are averaged to mono. Returns the levels after the block.
    AudioLevels feed(const void *data, std::size_t frames, int channels, SampleFormat format, int sampleRate);
    // The same for mono samples already in -1..1.
    AudioLevels feed(const float *mono, std::size_t frames, int sampleRate);

    // Forgets everything: a new track, or playback stopped.
    void reset();

private:
    // A second-order filter section. First-order ones were tried and
    // leak too much: a pure 60 Hz tone filled the "mid" band as well.
    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;
        double step(double x)
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    void designFilters(int sampleRate);

    // Filter memory is kept across blocks -- a block boundary is not a
    // boundary in the music. low: below 160 Hz. mid: 160 Hz to 2.5 kHz.
    // high: above 2.5 kHz.
    int m_sampleRate = 0;
    Biquad m_low;
    Biquad m_midHighPass;
    Biquad m_midLowPass;
    Biquad m_high;
    double m_previousLow = 0.0;
    // The loudest the whole signal has been lately: what 1.0 means.
    double m_reference = 0.0;
    AudioLevels m_levels;
    // A slow average of the low band, which a beat has to stand out from.
    double m_lowAverage = 0.0;
    double m_sinceBeatSeconds = 1.0;
    std::vector<float> m_mono;
};

}  // namespace seabass::domain
