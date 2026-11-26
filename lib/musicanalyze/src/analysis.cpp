// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "musicanalyze/analysis.hpp"

namespace musicanalyze
{

Analysis analyse(std::span<const float> samples, int sampleRateHz, TempoRange range)
{
    Analysis analysis;
    analysis.waveform = waveformFromSamples(samples, sampleRateHz);
    analysis.grid = gridFromSamples(samples, sampleRateHz, range);
    analysis.bpm = usualBpm(analysis.grid.beats);
    analysis.key = keyFromSamples(samples, sampleRateHz);
    analysis.revisions = revisions();
    return analysis;
}

}  // namespace musicanalyze
