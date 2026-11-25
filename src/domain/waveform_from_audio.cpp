// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/waveform_from_audio.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace seabass::domain
{

namespace
{

// A second-order filter in transposed direct form II, coefficients from
// the Audio EQ Cookbook with Q = 1/sqrt(2): a Butterworth low or high
// pass. Doubles throughout: at 150 Hz and 48 kHz the poles sit close to
// the unit circle, where floats lose the low end.
class Biquad
{
public:
    enum class Kind { LowPass, HighPass };

    Biquad(Kind kind, double cornerHz, int sampleRateHz)
    {
        const double w0 = 2.0 * std::numbers::pi * cornerHz / sampleRateHz;
        const double cosW = std::cos(w0);
        // sin(w0) / (2 Q), and 2 Q is sqrt(2).
        const double alpha = std::sin(w0) / std::numbers::sqrt2;
        const double a0 = 1.0 + alpha;
        if (kind == Kind::LowPass) {
            m_b0 = (1.0 - cosW) / 2.0 / a0;
            m_b1 = (1.0 - cosW) / a0;
        } else {
            m_b0 = (1.0 + cosW) / 2.0 / a0;
            m_b1 = -(1.0 + cosW) / a0;
        }
        m_b2 = m_b0;
        m_a1 = -2.0 * cosW / a0;
        m_a2 = (1.0 - alpha) / a0;
    }

    double process(double x)
    {
        const double y = m_b0 * x + m_z1;
        m_z1 = m_b1 * x - m_a1 * y + m_z2;
        m_z2 = m_b2 * x - m_a2 * y;
        return y;
    }

private:
    double m_b0 = 0.0, m_b1 = 0.0, m_b2 = 0.0, m_a1 = 0.0, m_a2 = 0.0;
    double m_z1 = 0.0, m_z2 = 0.0;
};

// A fourth-order Linkwitz-Riley filter: the same Butterworth twice.
class LinkwitzRiley
{
public:
    LinkwitzRiley(Biquad::Kind kind, double cornerHz, int sampleRateHz)
        : m_first(kind, cornerHz, sampleRateHz), m_second(kind, cornerHz, sampleRateHz)
    {
    }

    double process(double x) { return m_second.process(m_first.process(x)); }

private:
    Biquad m_first;
    Biquad m_second;
};

// The whole measurement, fed in pieces: the filters keep their state
// from one piece to the next, so a source read in chunks gives exactly
// what the same samples in one span give.
class WaveformMeter
{
public:
    explicit WaveformMeter(int sampleRateHz)
        : m_rate(sampleRateHz),
          m_low(Biquad::Kind::LowPass, WaveformLowMidEdgeHz, sampleRateHz),
          m_midHigh(Biquad::Kind::HighPass, WaveformLowMidEdgeHz, sampleRateHz),
          m_midLow(Biquad::Kind::LowPass, WaveformMidHighEdgeHz, sampleRateHz),
          m_high(Biquad::Kind::HighPass, WaveformMidHighEdgeHz, sampleRateHz)
    {
        m_entryEnd = entryStart(1);
    }

    void feed(std::span<const float> samples)
    {
        for (const float sample : samples) {
            const double x = sample;
            const double low = m_low.process(x);
            const double mid = m_midLow.process(m_midHigh.process(x));
            const double high = m_high.process(x);
            m_sumSquares[0] += low * low;
            m_sumSquares[1] += mid * mid;
            m_sumSquares[2] += high * high;
            m_peak = std::max(m_peak, std::abs(x));
            ++m_inEntry;
            ++m_total;
            if (m_total == m_entryEnd) {
                closeEntry();
            }
        }
    }

    AudioWaveform finish()
    {
        if (m_inEntry > 0) {
            closeEntry();
        }
        m_result.sampleRateHz = m_rate;
        m_result.sampleCount = m_total;
        return std::move(m_result);
    }

private:
    std::uint64_t entryStart(std::uint64_t entry) const
    {
        return entry * static_cast<std::uint64_t>(m_rate) / WaveformEntriesPerSecond;
    }

    void closeEntry()
    {
        const auto level = [&](double sumSquares) {
            const double rms = std::sqrt(sumSquares / static_cast<double>(m_inEntry));
            return static_cast<float>(std::min(1.0, rms * std::numbers::sqrt2));
        };
        m_result.entries.push_back({level(m_sumSquares[0]), level(m_sumSquares[1]), level(m_sumSquares[2]),
                                    static_cast<float>(std::min(1.0, m_peak))});
        m_sumSquares = {0.0, 0.0, 0.0};
        m_peak = 0.0;
        m_inEntry = 0;
        m_entryEnd = entryStart(m_result.entries.size() + 1);
    }

    int m_rate;
    LinkwitzRiley m_low;
    LinkwitzRiley m_midHigh;
    LinkwitzRiley m_midLow;
    LinkwitzRiley m_high;
    std::array<double, 3> m_sumSquares{0.0, 0.0, 0.0};
    double m_peak = 0.0;
    std::uint64_t m_inEntry = 0;
    std::uint64_t m_total = 0;
    std::uint64_t m_entryEnd = 0;
    AudioWaveform m_result;
};

}  // namespace

std::size_t waveformEntryCount(std::uint64_t sampleCount, int sampleRateHz)
{
    if (sampleRateHz <= 0) {
        return 0;
    }
    const auto rate = static_cast<std::uint64_t>(sampleRateHz);
    return static_cast<std::size_t>((sampleCount * WaveformEntriesPerSecond + rate - 1) / rate);
}

AudioWaveform waveformFromSamples(std::span<const float> samples, int sampleRateHz)
{
    if (sampleRateHz <= 0) {
        return {};
    }
    WaveformMeter meter(sampleRateHz);
    meter.feed(samples);
    return meter.finish();
}

AudioWaveform waveformFromPcm(PcmSource &source, std::string *error)
{
    const int rate = source.sampleRate();
    if (rate <= 0) {
        if (error != nullptr) {
            *error = source.error().empty() ? std::string("the source has no sample rate") : source.error();
        }
        return {};
    }
    WaveformMeter meter(rate);
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

std::vector<WaveformEntry> reduceWaveform(const std::vector<WaveformEntry> &entries, std::size_t columns)
{
    std::vector<WaveformEntry> out;
    if (entries.empty() || columns == 0) {
        return out;
    }
    out.reserve(columns);
    const std::size_t n = entries.size();
    for (std::size_t j = 0; j < columns; ++j) {
        const std::size_t from = j * n / columns;
        const std::size_t to = std::max(from + 1, (j + 1) * n / columns);
        double low = 0.0, mid = 0.0, high = 0.0, overall = 0.0;
        for (std::size_t i = from; i < to; ++i) {
            low += entries[i].low;
            mid += entries[i].mid;
            high += entries[i].high;
            overall += entries[i].overall;
        }
        const auto count = static_cast<double>(to - from);
        out.push_back({static_cast<float>(low / count), static_cast<float>(mid / count),
                       static_cast<float>(high / count), static_cast<float>(overall / count)});
    }
    return out;
}

}  // namespace seabass::domain
