// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "domain/pcm_source.hpp"

namespace seabass::domain
{

// A track's waveform measured from its audio, at the resolution a
// Pioneer player scrolls it by: 150 entries a second, the rate of
// rekordbox's PWV3, PWV5 and PWV7 sections. A first version, to be
// replaced behind the same interface: plain filters and plain levels,
// no tuning beyond the few numbers measured below.
//
// Three bands, split by Linkwitz-Riley crossovers (two second-order
// Butterworth filters in a row, so the bands sum back to the signal
// with no bump at an edge):
//
//   low   below 150 Hz       kick and bass
//   mid   150 Hz to 2 kHz    snare body, vocals, most melodic content
//   high  above 2 kHz        hats, cymbals, the top of a snare
//
// The edges come from comparing rekordbox 7's own PWV7 bytes with the
// audio of 28 tracks of a private collection, cut into the same 150 a
// second: the low byte follows the level below 100 Hz most closely and
// its correlation falls below the mid byte's between 130 and 160 Hz; the
// mid byte follows 250 Hz to 1.6 kHz and falls below the high byte
// between 1.6 and 2 kHz. Round numbers inside those ranges, not a fit.
constexpr int WaveformEntriesPerSecond = 150;
constexpr double WaveformLowMidEdgeHz = 150.0;
constexpr double WaveformMidHighEdgeHz = 2000.0;
// The lowest sample rate a waveform is measured at. Below it the 2 kHz
// edge comes too near half the rate, and at 4 kHz and under the filters
// no longer hold: every level would stick at 1. No file a DJ plays is
// sampled that low, so such a source gives an empty waveform.
constexpr int WaveformLowestSampleRateHz = 8000;

// One entry: each level 0 to 1.
struct WaveformEntry
{
    // Each band's RMS level, times the square root of 2 so a full-scale
    // sine in that band reads 1; louder clips at 1.
    float low = 0.0f;
    float mid = 0.0f;
    float high = 0.0f;
    // The loudest sample of the entry, whatever its frequency: how tall a
    // single-colour waveform is drawn there.
    float overall = 0.0f;
};

struct AudioWaveform
{
    // Entry i covers the samples from i * rate / 150 (rounded down) to
    // the next entry's first, so the last one may be short.
    std::vector<WaveformEntry> entries;
    int sampleRateHz = 0;
    std::uint64_t sampleCount = 0;
};

// How many entries `sampleCount` samples make: every started 1/150 s
// counts. rekordbox's PWV3 has exactly this many, with the sample count
// it stores for an MP3 in PVBR, on 1309 of 1312 MP3 tracks measured (the
// other three are one entry short, all on an exact boundary).
std::size_t waveformEntryCount(std::uint64_t sampleCount, int sampleRateHz);

// Measures a waveform in one pass. A sample rate under
// WaveformLowestSampleRateHz gives an empty waveform. A sample that is no
// number (a decoder's slip) counts as silence: left in, the filters would
// carry it through the rest of the track.
AudioWaveform waveformFromSamples(std::span<const float> samples, int sampleRateHz);

// The same from a source, read to its end; a source whose rate is too
// low gives an empty waveform and says so in `error`. The source's error, if it
// stopped on one, goes into `error` (when given) and the waveform covers
// what was read: a caller must check it, since a track cut short by a
// decode error is not the whole track.
AudioWaveform waveformFromPcm(PcmSource &source, std::string *error = nullptr);

// The waveform squeezed (or stretched) into `columns` columns, each the
// mean of the entries it covers, for the fixed-size previews (rekordbox
// writes 400, 100 and 1200 columns whatever the track's length). Column
// j covers entries j * n / columns up to (j + 1) * n / columns, at least
// one. Means rather than peaks: rekordbox's 1200-column PWV6 follows the
// mean of its own PWV7 over the same span (correlation 0.96, 0.81, 0.74
// for low, mid and high over 200 tracks) more closely than the peak
// (0.88, 0.49, 0.48). Empty in, or 0 columns: empty out.
std::vector<WaveformEntry> reduceWaveform(const std::vector<WaveformEntry> &entries, std::size_t columns);

}  // namespace seabass::domain
