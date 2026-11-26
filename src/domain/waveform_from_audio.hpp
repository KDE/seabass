// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "domain/pcm_source.hpp"
#include "musicanalyze/waveform.hpp"

namespace seabass::domain
{

// The waveform measured from audio is the analysis library's
// (lib/musicanalyze, waveform.hpp, where what it measures is written
// down). Its names under the ones Seabass has used for them.
using musicanalyze::AudioWaveform;
using musicanalyze::reduceWaveform;
using musicanalyze::WaveformEntriesPerSecond;
using musicanalyze::WaveformEntry;
using musicanalyze::waveformEntryCount;
using musicanalyze::waveformFromSamples;
using musicanalyze::WaveformLowestSampleRateHz;
using musicanalyze::WaveformLowMidEdgeHz;
using musicanalyze::WaveformMidHighEdgeHz;

// The waveform of a source, read to its end in pieces; a source whose
// rate is too low gives an empty waveform and says so in `error`. The
// source's error, if it stopped on one, goes into `error` (when given)
// and the waveform covers what was read: a caller must check it, since a
// track cut short by a decode error is not the whole track.
inline AudioWaveform waveformFromPcm(PcmSource &source, std::string *error = nullptr)
{
    const int rate = source.sampleRate();
    if (rate <= 0) {
        if (error != nullptr) {
            *error = source.error().empty() ? std::string("the source has no sample rate") : source.error();
        }
        return {};
    }
    if (rate < WaveformLowestSampleRateHz) {
        if (error != nullptr) {
            *error = "the sample rate is too low for a waveform";
        }
        return {};
    }
    musicanalyze::WaveformMeter meter(rate);
    std::vector<float> chunk(16384);
    for (;;) {
        const std::size_t got = source.read(chunk);
        if (got == 0) {
            break;
        }
        meter.feed(std::span<const float>(chunk.data(), got));
    }
    if (error != nullptr) {
        *error = source.error();
    }
    return meter.finish();
}

}  // namespace seabass::domain
