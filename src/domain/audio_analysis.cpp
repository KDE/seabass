// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/audio_analysis.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace seabass::domain
{

namespace
{

// A minute over the middle gap. The middle, not the mean: a followed
// grid's few long gaps over a breakdown would pull a mean down.
double usualBpm(const std::vector<Beat> &beats)
{
    if (beats.size() < 2) {
        return 0.0;
    }
    std::vector<double> gaps;
    gaps.reserve(beats.size() - 1);
    for (std::size_t i = 1; i < beats.size(); ++i) {
        gaps.push_back(beats[i].timeMs - beats[i - 1].timeMs);
    }
    const auto middle = gaps.begin() + static_cast<std::ptrdiff_t>(gaps.size() / 2);
    std::nth_element(gaps.begin(), middle, gaps.end());
    return *middle > 0.0 ? 60000.0 / *middle : 0.0;
}

}  // namespace

AudioAnalysis analyseAudio(std::span<const float> samples, int sampleRateHz, TempoRange range)
{
    AudioAnalysis analysis;
    analysis.waveform = waveformFromSamples(samples, sampleRateHz);
    analysis.grid = SeabassGridAnalyser(range).analyseSamples(samples, sampleRateHz);
    analysis.bpm = usualBpm(analysis.grid.beats);
    analysis.key = keyFromSamples(samples, sampleRateHz);
    return analysis;
}

}  // namespace seabass::domain
