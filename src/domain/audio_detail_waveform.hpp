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
// 150 columns a second, rekordbox's scrolling resolution. Each column is
// the peak of three bands, split by two one-pole low-pass filters:
//   low   below about 200 Hz  (the 200 Hz low-pass)
//   mid   between the two      (2 kHz low-pass minus 200 Hz low-pass)
//   high  above about 2 kHz    (the input minus the 2 kHz low-pass)
// and all three are scaled by one factor so the loudest band of the
// loudest column is 1: the bands keep their sizes relative to each
// other, which is what a layered drawing shows.
class AudioDetailWaveformBuilder
{
public:
    static constexpr double ColumnsPerSecond = 150.0;
    static constexpr double LowCutHz = 200.0;
    static constexpr double HighCutHz = 2000.0;

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
    unsigned long long m_samples = 0;
    unsigned long long m_columnEnd = 0;
    bool m_columnOpen = false;
    WaveformColumn m_peak;
    std::vector<WaveformColumn> m_columns;
};

}  // namespace seabass::domain
