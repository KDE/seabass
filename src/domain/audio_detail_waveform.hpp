// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <vector>

#include "domain/waveform.hpp"

namespace seabass::domain
{

// A scrolling waveform made from the audio itself, for an Engine track in
// the zoomed popup: Engine stores only an overview (about a thousand
// columns to the track), and a Denon player computes its scrolling
// waveform from the audio as it decodes it. Built from mono samples on
// the decoder's timeline, so column 0 starts at that decoder's sample 0.
//
// 150 columns a second, rekordbox's scrolling resolution. Three bands,
// split by two one-pole low-pass filters:
//   low   below about 200 Hz  (the 200 Hz low-pass)
//   mid   between the two      (2 kHz low-pass minus 200 Hz low-pass)
//   high  above about 2 kHz    (the input minus the 2 kHz low-pass)
// Each band goes through an envelope follower, as a player's display
// does: it rises at once to any sample louder than it (so an onset lights
// the column it is in, never a later one, which is what judging a grid
// by the kicks needs) and falls exponentially after, so a kick is a sharp
// attack and a smooth decay instead of the jitter of a raw per-column
// peak. A column is the follower's peak within it. All three bands are
// scaled by one factor so the loudest band of the loudest column is 1:
// the bands keep their sizes relative to each other, which is what a
// layered drawing shows.
class AudioDetailWaveformBuilder
{
public:
    static constexpr double ColumnsPerSecond = 150.0;
    static constexpr double LowCutHz = 200.0;
    static constexpr double HighCutHz = 2000.0;
    // The followers' release, the time to fall to 1/e: long enough for a
    // low band to ride over the gaps between the cycles of a bass note
    // (20 ms at 50 Hz) and for a kick to decay as a swell; shorter for the
    // higher bands, whose hits are shorter.
    static constexpr double LowReleaseMs = 80.0;
    static constexpr double MidReleaseMs = 60.0;
    static constexpr double HighReleaseMs = 40.0;

    explicit AudioDetailWaveformBuilder(int sampleRate);

    // Room for this many seconds of columns, so a long track's are not
    // grown by doubling. Only ever grows.
    void reserveSeconds(double seconds);

    // The next samples, mono, -1..1.
    void push(const float *mono, std::size_t count);

    // Every column so far, the last one however short. Empty (and
    // msPerColumn 0) when no sample came.
    DetailWaveform finish();

private:
    void closeColumn();

    int m_sampleRate;
    double m_lowAlpha;
    double m_highAlpha;
    double m_low = 0.0;      // the 200 Hz low-pass's state
    double m_belowHigh = 0.0;  // the 2 kHz low-pass's state
    // Per sample, what each follower keeps of itself.
    double m_lowRelease;
    double m_midRelease;
    double m_highRelease;
    WaveformColumn m_envelope;
    unsigned long long m_samples = 0;
    unsigned long long m_columnEnd = 0;
    bool m_columnOpen = false;
    WaveformColumn m_peak;
    std::vector<WaveformColumn> m_columns;
};

}  // namespace seabass::domain
