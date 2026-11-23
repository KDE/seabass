// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/audio_detail_waveform.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace seabass::domain
{

namespace
{

double onePoleAlpha(double cutoffHz, int sampleRate)
{
    return 1.0 - std::exp(-2.0 * std::numbers::pi * cutoffHz / static_cast<double>(sampleRate));
}

double releaseFactor(double releaseMs, int sampleRate)
{
    return std::exp(-1000.0 / (releaseMs * static_cast<double>(sampleRate)));
}

// Where column `index` ends, in samples: exact at every rate, so the
// columns never drift from the timeline however long the track. Sample s
// is in column floor(s * 150 / rate), so column k ends at the first
// sample at or after (k + 1) * rate / 150.
unsigned long long columnEnd(unsigned long long index, int sampleRate)
{
    const auto perSecond = static_cast<unsigned long long>(AudioDetailWaveformBuilder::ColumnsPerSecond);
    return ((index + 1) * static_cast<unsigned long long>(sampleRate) + perSecond - 1) / perSecond;
}

}  // namespace

AudioDetailWaveformBuilder::AudioDetailWaveformBuilder(int sampleRate)
    : m_sampleRate(std::max(1, sampleRate))
    , m_lowAlpha(onePoleAlpha(LowCutHz, m_sampleRate))
    , m_highAlpha(onePoleAlpha(HighCutHz, m_sampleRate))
    , m_lowRelease(releaseFactor(LowReleaseMs, m_sampleRate))
    , m_midRelease(releaseFactor(MidReleaseMs, m_sampleRate))
    , m_highRelease(releaseFactor(HighReleaseMs, m_sampleRate))
    , m_columnEnd(columnEnd(0, m_sampleRate))
{
}

void AudioDetailWaveformBuilder::closeColumn()
{
    m_columns.push_back(m_peak);
    m_peak = {};
    m_columnOpen = false;
    m_columnEnd = columnEnd(m_columns.size(), m_sampleRate);
}

void AudioDetailWaveformBuilder::reserveSeconds(double seconds)
{
    if (seconds > 0.0) {
        m_columns.reserve(static_cast<std::size_t>(std::ceil(seconds * ColumnsPerSecond)) + 1);
    }
}

void AudioDetailWaveformBuilder::push(const float *mono, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        const double x = mono[i];
        m_low += m_lowAlpha * (x - m_low);
        m_belowHigh += m_highAlpha * (x - m_belowHigh);
        m_envelope.low = std::max(std::fabs(m_low), m_envelope.low * m_lowRelease);
        m_envelope.mid = std::max(std::fabs(m_belowHigh - m_low), m_envelope.mid * m_midRelease);
        m_envelope.high = std::max(std::fabs(x - m_belowHigh), m_envelope.high * m_highRelease);
        m_peak.low = std::max(m_peak.low, m_envelope.low);
        m_peak.mid = std::max(m_peak.mid, m_envelope.mid);
        m_peak.high = std::max(m_peak.high, m_envelope.high);
        m_columnOpen = true;
        if (++m_samples >= m_columnEnd) {
            closeColumn();
        }
    }
}

DetailWaveform AudioDetailWaveformBuilder::finish()
{
    if (m_columnOpen) {
        closeColumn();
    }
    DetailWaveform detail;
    if (m_columns.empty()) {
        return detail;
    }
    double loudest = 0.0;
    for (const WaveformColumn &column : m_columns) {
        loudest = std::max({loudest, column.low, column.mid, column.high});
    }
    if (loudest > 0.0) {
        for (WaveformColumn &column : m_columns) {
            column.low /= loudest;
            column.mid /= loudest;
            column.high /= loudest;
        }
    }
    detail.columns = std::move(m_columns);
    m_columns = {};
    detail.msPerColumn = 1000.0 / ColumnsPerSecond;
    return detail;
}

}  // namespace seabass::domain
