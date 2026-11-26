// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "musicanalyze/analysis.hpp"

namespace musicanalyze
{

Analyser::Analyser(int sampleRateHz, TempoRange range) : m_grid(sampleRateHz, range), m_key(sampleRateHz), m_waveform(sampleRateHz) {}

void Analyser::feed(std::span<const float> samples)
{
    m_waveform.feed(samples);
    m_grid.feed(samples);
    m_key.feed(samples);
}

Analysis Analyser::finish()
{
    Analysis analysis;
    analysis.waveform = m_waveform.finish();
    analysis.grid = m_grid.finish();
    analysis.bpm = usualBpm(analysis.grid.beats);
    analysis.key = m_key.finish();
    analysis.revisions = revisions();
    return analysis;
}

Analysis analyse(std::span<const float> samples, int sampleRateHz, TempoRange range)
{
    Analyser analyser(sampleRateHz, range);
    analyser.feed(samples);
    return analyser.finish();
}

}  // namespace musicanalyze
