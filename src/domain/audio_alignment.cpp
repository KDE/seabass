// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/audio_alignment.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace seabass::domain::alignment
{

void fft(std::vector<std::complex<double>> &data, bool inverse)
{
    const size_t n = data.size();
    if (n < 2) {
        return;
    }
    // Bit-reversed order first, then the butterflies in place.
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }
    // One table of twiddles for the largest stage, read with a stride by
    // the smaller ones. Each one computed from its own angle rather than
    // by multiplying the previous one on: over a million steps the
    // repeated product drifts, and a drifting twiddle is a drifting lag.
    const double sign = inverse ? 1.0 : -1.0;
    std::vector<std::complex<double>> twiddle(n / 2);
    for (size_t k = 0; k < n / 2; ++k) {
        twiddle[k] = std::polar(1.0, sign * 2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(n));
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const size_t half = len / 2;
        const size_t stride = n / len;
        for (size_t start = 0; start < n; start += len) {
            for (size_t k = 0; k < half; ++k) {
                const std::complex<double> t = twiddle[k * stride] * data[start + k + half];
                const std::complex<double> u = data[start + k];
                data[start + k] = u + t;
                data[start + k + half] = u - t;
            }
        }
    }
    if (inverse) {
        const double scale = 1.0 / static_cast<double>(n);
        for (auto &value : data) {
            value *= scale;
        }
    }
}

Resampler::Resampler(double inRate, double outRate) : m_ratio(outRate / inRate) {}

void Resampler::push(float sample)
{
    // This input sample's interval, in output samples: [lo, hi).
    const double k = static_cast<double>(m_inputIndex++);
    const double lo = (k - 0.5) * m_ratio;
    const double hi = (k + 0.5) * m_ratio;
    // Output sample n covers [n - 0.5, n + 0.5).
    const long long first = std::max(0LL, static_cast<long long>(std::floor(lo + 0.5)));
    const long long last = static_cast<long long>(std::ceil(hi + 0.5)) - 1;
    for (long long n = first; n <= last; ++n) {
        const double overlap = std::min(hi, static_cast<double>(n) + 0.5) - std::max(lo, static_cast<double>(n) - 0.5);
        if (overlap <= 0.0) {
            continue;
        }
        while (m_base + static_cast<long long>(m_sum.size()) <= n) {
            m_sum.push_back(0.0);
            m_weight.push_back(0.0);
        }
        const size_t slot = static_cast<size_t>(n - m_base);
        m_sum[slot] += overlap * static_cast<double>(sample);
        m_weight[slot] += overlap;
    }
    // Every output sample whose interval ends at or before this input
    // sample's end has had all the input it will ever get.
    const long long complete = static_cast<long long>(std::floor(hi - 0.5));
    while (!m_sum.empty() && m_base <= complete) {
        m_out.push_back(m_weight.front() > 0.0 ? static_cast<float>(m_sum.front() / m_weight.front()) : 0.0f);
        m_sum.pop_front();
        m_weight.pop_front();
        ++m_base;
    }
}

std::vector<float> Resampler::finish()
{
    // The last output samples are only partly covered by input: their
    // average is over the part there is, the weight says how much.
    while (!m_sum.empty()) {
        if (m_weight.front() > 0.0) {
            m_out.push_back(static_cast<float>(m_sum.front() / m_weight.front()));
        }
        m_sum.pop_front();
        m_weight.pop_front();
        ++m_base;
    }
    return std::move(m_out);
}

double shiftSearchSeconds(double compareAudioSeconds)
{
    return std::max(MinShiftSearchSeconds, compareAudioSeconds + ShiftSearchMarginSeconds);
}

std::optional<CorrelationPeak> correlationPeak(std::span<const float> a, std::span<const float> b,
                                               size_t minOverlap, std::optional<LagRange> range,
                                               size_t rivalSeparation)
{
    const size_t na = a.size();
    const size_t nb = b.size();
    if (na == 0 || nb == 0 || minOverlap == 0 || std::min(na, nb) < minOverlap) {
        return std::nullopt;
    }
    size_t n = 1;
    while (n < na + nb - 1) {
        n <<= 1;
    }

    // Both real signals in one complex transform, a in the real part and
    // b in the imaginary, and their spectra pulled apart afterwards:
    // half the transforms of doing them one by one, on windows of two
    // million points.
    std::vector<std::complex<double>> z(n);
    for (size_t i = 0; i < na; ++i) {
        z[i].real(a[i]);
    }
    for (size_t i = 0; i < nb; ++i) {
        z[i].imag(b[i]);
    }
    fft(z, false);
    // conj(A[k]) * B[k], whose inverse is sum_i a[i] * b[i + lag] at
    // index lag (and at n - m for lag -m). Done in pairs k, n - k,
    // because each needs both of the packed spectrum's entries.
    for (size_t k = 0; k <= n / 2; ++k) {
        const size_t m = (n - k) % n;
        const std::complex<double> zk = z[k];
        const std::complex<double> zm = z[m];
        const std::complex<double> ak = (zk + std::conj(zm)) * 0.5;
        const std::complex<double> bk = (zk - std::conj(zm)) * std::complex<double>(0.0, -0.5);
        const std::complex<double> am = (zm + std::conj(zk)) * 0.5;
        const std::complex<double> bm = (zm - std::conj(zk)) * std::complex<double>(0.0, -0.5);
        z[k] = std::conj(ak) * bk;
        z[m] = std::conj(am) * bm;
    }
    fft(z, true);

    const long long lna = static_cast<long long>(na);
    const long long lnb = static_cast<long long>(nb);
    const long long need = static_cast<long long>(minOverlap);
    auto overlapAt = [&](long long lag) { return std::min(lna, lnb - lag) - std::max(0LL, -lag); };

    // Each side's energy over any stretch, from running sums, so the
    // coefficient at every lag costs two subtractions.
    std::vector<double> energyBeforeA(na + 1, 0.0);
    std::vector<double> energyBeforeB(nb + 1, 0.0);
    for (size_t i = 0; i < na; ++i) {
        energyBeforeA[i + 1] = energyBeforeA[i] + static_cast<double>(a[i]) * static_cast<double>(a[i]);
    }
    for (size_t i = 0; i < nb; ++i) {
        energyBeforeB[i + 1] = energyBeforeB[i] + static_cast<double>(b[i]) * static_cast<double>(b[i]);
    }
    // An overlap with less energy than this share of its side's whole is
    // silence as far as the running sums can tell: their rounding is then
    // as large as what is left, and the quotient would be noise.
    constexpr double SilentShare = 1e-9;
    const double silentA = energyBeforeA[na] * SilentShare;
    const double silentB = energyBeforeB[nb] * SilentShare;
    auto coefficientAt = [&](long long lag) {
        const long long from = std::max(0LL, -lag);
        const long long to = std::min(lna, lnb - lag);
        const double energyA = energyBeforeA[static_cast<size_t>(to)] - energyBeforeA[static_cast<size_t>(from)];
        const double energyB =
            energyBeforeB[static_cast<size_t>(to + lag)] - energyBeforeB[static_cast<size_t>(from + lag)];
        if (energyA <= silentA || energyB <= silentB || energyA <= 0.0 || energyB <= 0.0) {
            return 0.0;
        }
        const size_t index = lag >= 0 ? static_cast<size_t>(lag) : n - static_cast<size_t>(-lag);
        return z[index].real() / std::sqrt(energyA * energyB);
    };

    // The lags to look at: the range asked for, cut to those with enough
    // overlap. Overlap shrinks away from the middle on both sides, so
    // this is one stretch.
    long long lowest = -(lna - 1);
    long long highest = lnb - 1;
    if (range) {
        lowest = std::max(lowest, range->lowest);
        highest = std::min(highest, range->highest);
    }
    while (lowest <= highest && overlapAt(lowest) < need) {
        ++lowest;
    }
    while (highest >= lowest && overlapAt(highest) < need) {
        --highest;
    }
    if (lowest > highest) {
        return std::nullopt;
    }
    std::vector<double> coefficient(static_cast<size_t>(highest - lowest + 1));
    size_t best = 0;
    for (size_t k = 0; k < coefficient.size(); ++k) {
        coefficient[k] = coefficientAt(lowest + static_cast<long long>(k));
        if (coefficient[k] > coefficient[best]) {
            best = k;
        }
    }
    const long long bestLag = lowest + static_cast<long long>(best);

    // A rival: any other local peak, or either end of the stretch (a peak
    // may lie just past it), far enough from the best not to be its own
    // shoulder and nearly as high.
    bool ambiguous = coefficient.size() > 1 && (best == 0 || best + 1 == coefficient.size());
    const double rivalLevel = RivalShare * coefficient[best];
    for (size_t k = 0; k < coefficient.size() && !ambiguous; ++k) {
        const size_t distance = k > best ? k - best : best - k;
        if (distance <= rivalSeparation || coefficient[k] < rivalLevel) {
            continue;
        }
        const bool risesFromLeft = k == 0 || coefficient[k] >= coefficient[k - 1];
        const bool fallsToRight = k + 1 == coefficient.size() || coefficient[k] >= coefficient[k + 1];
        if (risesFromLeft && fallsToRight) {
            ambiguous = true;
        }
    }

    // The coefficient at that lag, straight from the samples: the
    // transform found where to look, and the number that decides
    // anything is worked out exactly.
    const long long from = std::max(0LL, -bestLag);
    const long long to = std::min(lna, lnb - bestLag);
    double cross = 0.0;
    double energyA = 0.0;
    double energyB = 0.0;
    for (long long i = from; i < to; ++i) {
        const double x = a[static_cast<size_t>(i)];
        const double y = b[static_cast<size_t>(i + bestLag)];
        cross += x * y;
        energyA += x * x;
        energyB += y * y;
    }
    if (energyA <= 0.0 || energyB <= 0.0) {
        return std::nullopt;
    }
    return CorrelationPeak{bestLag, cross / std::sqrt(energyA * energyB), ambiguous};
}

std::optional<double> cleanShiftMs(const std::vector<WindowShift> &windows)
{
    if (windows.empty()) {
        return std::nullopt;
    }
    std::vector<double> shifts;
    for (const auto &window : windows) {
        if (window.ambiguous || !(window.normalised >= MinNormalisedPeak)) {
            return std::nullopt;
        }
        shifts.push_back(window.shiftMs);
    }
    std::sort(shifts.begin(), shifts.end());
    if (shifts.back() - shifts.front() > MaxDisagreementMs) {
        return std::nullopt;
    }
    const size_t mid = shifts.size() / 2;
    return shifts.size() % 2 == 1 ? shifts[mid] : (shifts[mid - 1] + shifts[mid]) / 2.0;
}

std::optional<std::vector<WindowShift>> windowShifts(std::span<const float> a, std::span<const float> b,
                                                     double rate, double maxShiftSeconds)
{
    if (rate <= 0.0 || !(maxShiftSeconds >= 0.0)) {
        return std::nullopt;
    }
    const size_t window = static_cast<size_t>(std::llround(WindowSeconds * rate));
    const size_t minOverlap = static_cast<size_t>(std::ceil(MinOverlapSeconds * rate));
    const size_t na = a.size();
    const size_t nb = b.size();
    if (std::min(na, nb) < minOverlap) {
        return std::nullopt;
    }

    struct Window
    {
        size_t startA, lengthA, startB, lengthB;
    };
    const size_t shorter = std::min(na, nb);
    const size_t middle = shorter / 2 > window / 2 ? shorter / 2 - window / 2 : 0;
    const Window windows[] = {
        {0, std::min(window, na), 0, std::min(window, nb)},
        // Both from the same index, half of the shorter file: the
        // shift there is the lag itself.
        {middle, std::min(window, na - middle), middle, std::min(window, nb - middle)},
        // Each file's own last stretch: a copy with a longer tail has
        // its end window further in, and the lag says where the two meet
        // counted from there.
        {na - std::min(window, na), std::min(window, na), nb - std::min(window, nb), std::min(window, nb)},
    };

    const long long maxShift = std::llround(maxShiftSeconds * rate);
    const size_t rivalSeparation = static_cast<size_t>(std::ceil(RivalSeparationMs * rate / 1000.0));
    std::vector<WindowShift> shifts;
    for (const Window &w : windows) {
        // The shift is the lag plus where b's window starts minus where
        // a's does, so the shifts searched are this range of lags.
        const long long startDifference = static_cast<long long>(w.startB) - static_cast<long long>(w.startA);
        const LagRange range{-maxShift - startDifference, maxShift - startDifference};
        const auto peak = correlationPeak(a.subspan(w.startA, w.lengthA), b.subspan(w.startB, w.lengthB), minOverlap,
                                          range, rivalSeparation);
        if (!peak) {
            return std::nullopt;
        }
        // From the window back to the files: a sound at window index i
        // in a is at file index startA + i, in b at startB + i + lag.
        const double samples = static_cast<double>(peak->lag) + static_cast<double>(w.startB)
            - static_cast<double>(w.startA);
        shifts.push_back({samples * 1000.0 / rate, peak->normalised, peak->ambiguous});
    }
    return shifts;
}

std::optional<double> measureShiftMs(std::span<const float> a, std::span<const float> b, double rate,
                                     double maxShiftSeconds)
{
    const auto windows = windowShifts(a, b, rate, maxShiftSeconds);
    if (!windows) {
        return std::nullopt;
    }
    return cleanShiftMs(*windows);
}

}  // namespace seabass::domain::alignment
