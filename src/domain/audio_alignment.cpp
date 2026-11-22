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

std::vector<float> Resampler::drain()
{
    std::vector<float> out;
    out.swap(m_out);
    return out;
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

    // Every lag with enough overlap is looked at, not only those in
    // `range`: a true peak just outside the range would otherwise go
    // unseen, and an alias of it inside, a phrase away, would win with no
    // rival to doubt it. Overlap shrinks away from the middle on both
    // sides, so this is one stretch.
    long long lowest = -(lna - 1);
    long long highest = lnb - 1;
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

    // Not known when the best lies outside the range asked for (a shift
    // further out than is believed), or at either end of the stretch (a
    // peak may lie just past it). A rival: any other local peak anywhere
    // in the stretch, far enough from the best not to be its own shoulder
    // and nearly as high.
    bool ambiguous = (coefficient.size() > 1 && (best == 0 || best + 1 == coefficient.size()))
        || (range && (bestLag < range->lowest || bestLag > range->highest));
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

namespace
{

struct Window
{
    size_t startA, lengthA, startB, lengthB;
};

// The three windows of a pair of files na and nb samples long: the first
// WindowSeconds of each, the WindowSeconds around half of the shorter
// file in both, and the last WindowSeconds of each.
std::vector<Window> windowsFor(size_t na, size_t nb, double rate)
{
    const size_t window = static_cast<size_t>(std::llround(WindowSeconds * rate));
    const size_t shorter = std::min(na, nb);
    const size_t middle = shorter / 2 > window / 2 ? shorter / 2 - window / 2 : 0;
    return {
        {0, std::min(window, na), 0, std::min(window, nb)},
        // Both from the same index, half of the shorter file: the
        // shift there is the lag itself.
        {middle, std::min(window, na - middle), middle, std::min(window, nb - middle)},
        // Each file's own last stretch: a copy with a longer tail has
        // its end window further in, and the lag says where the two meet
        // counted from there.
        {na - std::min(window, na), std::min(window, na), nb - std::min(window, nb), std::min(window, nb)},
    };
}

template<typename SliceA, typename SliceB>
std::optional<std::vector<WindowShift>> shiftsOver(size_t na, size_t nb, const SliceA &sliceA, const SliceB &sliceB,
                                                   double rate, double maxShiftSeconds)
{
    if (rate <= 0.0 || !(maxShiftSeconds >= 0.0)) {
        return std::nullopt;
    }
    const size_t minOverlap = static_cast<size_t>(std::ceil(MinOverlapSeconds * rate));
    if (std::min(na, nb) < minOverlap) {
        return std::nullopt;
    }
    const long long maxShift = std::llround(maxShiftSeconds * rate);
    const size_t rivalSeparation = static_cast<size_t>(std::ceil(RivalSeparationMs * rate / 1000.0));
    std::vector<WindowShift> shifts;
    const std::vector<Window> windows = windowsFor(na, nb, rate);
    for (size_t k = 0; k < windows.size(); ++k) {
        const Window &w = windows[k];
        // Files shorter than a window give the same window three times:
        // measured once.
        if (k > 0 && w.startA == windows[k - 1].startA && w.lengthA == windows[k - 1].lengthA
            && w.startB == windows[k - 1].startB && w.lengthB == windows[k - 1].lengthB) {
            shifts.push_back(shifts.back());
            continue;
        }
        const std::optional<std::span<const float>> windowA = sliceA(w.startA, w.lengthA);
        const std::optional<std::span<const float>> windowB = sliceB(w.startB, w.lengthB);
        if (!windowA || !windowB) {
            return std::nullopt;
        }
        // The shift is the lag plus where b's window starts minus where
        // a's does, so the shifts searched are this range of lags.
        const long long startDifference = static_cast<long long>(w.startB) - static_cast<long long>(w.startA);
        const LagRange range{-maxShift - startDifference, maxShift - startDifference};
        const auto peak = correlationPeak(*windowA, *windowB, minOverlap, range, rivalSeparation);
        if (!peak) {
            return std::nullopt;
        }
        // From the window back to the files: a sound at window index i
        // in a is at file index startA + i, in b at startB + i + lag.
        const double samples = static_cast<double>(peak->lag) + static_cast<double>(startDifference);
        shifts.push_back({samples * 1000.0 / rate, peak->normalised, peak->ambiguous});
    }
    return shifts;
}

auto wholeSlicer(std::span<const float> whole)
{
    return [whole](size_t start, size_t length) -> std::optional<std::span<const float>> {
        return whole.subspan(start, length);
    };
}

auto keptSlicer(const KeptSamples &kept)
{
    return [&kept](size_t start, size_t length) { return kept.slice(start, length); };
}

// Whether two of a pair's windows share samples in either file: then they
// are not three looks at the music, and agreeing proves nothing.
bool windowsShareSamples(size_t na, size_t nb, double rate)
{
    const std::vector<Window> windows = windowsFor(na, nb, rate);
    auto overlap = [](size_t startX, size_t lengthX, size_t startY, size_t lengthY) {
        return startX < startY + lengthY && startY < startX + lengthX;
    };
    for (size_t i = 0; i < windows.size(); ++i) {
        for (size_t j = i + 1; j < windows.size(); ++j) {
            if (overlap(windows[i].startA, windows[i].lengthA, windows[j].startA, windows[j].lengthA)
                || overlap(windows[i].startB, windows[i].lengthB, windows[j].startB, windows[j].lengthB)) {
                return true;
            }
        }
    }
    return false;
}

// The overlap of the two files at `shiftMs`, in parts of PartSeconds (at
// least three), each part with sound in it correlating at least `floor`.
// A part all but silent on either side (under a hundredth of its share of
// the energy: a silent intro) says nothing either way and is passed over,
// but at least two parts must have sound. False too when the files were
// not kept whole.
template<typename SliceA, typename SliceB>
bool holdsInEveryPart(size_t na, size_t nb, const SliceA &sliceA, const SliceB &sliceB, double shiftMs, double rate,
                      double floor)
{
    const auto a = sliceA(0, na);
    const auto b = sliceB(0, nb);
    if (!a || !b) {
        return false;
    }
    const long long lag = std::llround(shiftMs * rate / 1000.0);
    const long long from = std::max(0LL, -lag);
    const long long to = std::min(static_cast<long long>(na), static_cast<long long>(nb) - lag);
    const long long partLength = std::max(1LL, std::llround(PartSeconds * rate));
    const long long parts = std::max(3LL, (to - from) / partLength);
    if (to - from < parts) {
        return false;
    }
    struct Sums
    {
        double cross = 0.0, energyA = 0.0, energyB = 0.0;
    };
    std::vector<Sums> sums(static_cast<size_t>(parts));
    Sums all;
    for (long long k = 0; k < parts; ++k) {
        const long long begin = from + k * (to - from) / parts;
        const long long end = from + (k + 1) * (to - from) / parts;
        Sums &part = sums[static_cast<size_t>(k)];
        for (long long i = begin; i < end; ++i) {
            const double x = (*a)[static_cast<size_t>(i)];
            const double y = (*b)[static_cast<size_t>(i + lag)];
            part.cross += x * y;
            part.energyA += x * x;
            part.energyB += y * y;
        }
        all.energyA += part.energyA;
        all.energyB += part.energyB;
    }
    constexpr double SilentShare = 0.01;
    const double share = 1.0 / static_cast<double>(parts);
    int heard = 0;
    for (const Sums &part : sums) {
        if (part.energyA <= SilentShare * share * all.energyA || part.energyB <= SilentShare * share * all.energyB) {
            continue;
        }
        ++heard;
        if (part.cross / std::sqrt(part.energyA * part.energyB) < floor) {
            return false;
        }
    }
    return heard >= 2;
}

template<typename SliceA, typename SliceB>
std::optional<double> measureOver(size_t na, size_t nb, const SliceA &sliceA, const SliceB &sliceB, double rate,
                                  double maxShiftSeconds)
{
    const auto windows = shiftsOver(na, nb, sliceA, sliceB, rate, maxShiftSeconds);
    if (!windows) {
        return std::nullopt;
    }
    const std::optional<double> shift = cleanShiftMs(*windows);
    if (!shift || !windowsShareSamples(na, nb, rate)) {
        return shift;
    }
    double weakest = 1.0;
    for (const WindowShift &window : *windows) {
        weakest = std::min(weakest, window.normalised);
    }
    if (!holdsInEveryPart(na, nb, sliceA, sliceB, *shift, rate, PartShare * weakest)) {
        return std::nullopt;
    }
    return shift;
}

}  // namespace

std::optional<std::span<const float>> KeptSamples::slice(size_t start, size_t length) const
{
    for (const Piece &piece : pieces) {
        if (start >= piece.start && start + length <= piece.start + piece.samples.size()) {
            return std::span<const float>(piece.samples).subspan(start - piece.start, length);
        }
    }
    return std::nullopt;
}

size_t KeptSamples::bytes() const
{
    size_t samples = 0;
    for (const Piece &piece : pieces) {
        samples += piece.samples.capacity();
    }
    return samples * sizeof(float);
}

KeptSamples keepAll(std::vector<float> samples)
{
    KeptSamples kept;
    kept.total = samples.size();
    samples.shrink_to_fit();
    kept.pieces.push_back({0, std::move(samples)});
    return kept;
}

KeptSamplesBuilder::KeptSamplesBuilder(double rate)
    : m_window(static_cast<size_t>(std::llround(WindowSeconds * rate))),
      m_reach(static_cast<size_t>(std::llround(MiddleReachSeconds * rate)))
{
}

void KeptSamplesBuilder::append(std::span<const float> samples)
{
    if (samples.empty()) {
        return;
    }
    m_blocks.push_back({m_total, std::vector<float>(samples.begin(), samples.end())});
    m_total += samples.size();
    m_held += samples.size();
    m_mostHeld = std::max(m_mostHeld, m_held);
    letGo();
}

void KeptSamplesBuilder::letGo()
{
    // However long the file turns out to be, its middle window starts no
    // earlier than this: half of what has arrived, less half a window and
    // the reach. A block wholly before it and wholly after the first
    // window can never be needed.
    const size_t half = m_total / 2;
    const size_t needed = half > m_window / 2 + m_reach ? half - m_window / 2 - m_reach : 0;
    // Blocks are in order, the ones let go of already gone: the ones to let
    // go of now are a run after the first window's, ending at the first
    // block that reaches `needed`.
    auto first = m_blocks.begin();
    while (first != m_blocks.end() && first->start < m_window) {
        ++first;
    }
    auto last = first;
    while (last != m_blocks.end() && last->start + last->samples.size() <= needed) {
        m_held -= last->samples.size();
        ++last;
    }
    m_blocks.erase(first, last);
}

KeptSamples KeptSamplesBuilder::finish()
{
    KeptSamples kept;
    kept.total = m_total;
    if (m_total == 0) {
        return kept;
    }
    // The stretches to keep, in order, joined where they touch.
    const size_t half = m_total / 2;
    const size_t middleFrom = half > m_window / 2 + m_reach ? half - m_window / 2 - m_reach : 0;
    const size_t middleTo = std::min(m_total, half + m_window / 2 + m_reach + 1);
    std::vector<std::pair<size_t, size_t>> stretches{
        {0, std::min(m_window, m_total)},
        {middleFrom, middleTo},
        {m_total - std::min(m_window, m_total), m_total},
    };
    std::vector<std::pair<size_t, size_t>> joined;
    for (const auto &stretch : stretches) {
        if (!joined.empty() && stretch.first <= joined.back().second) {
            joined.back().second = std::max(joined.back().second, stretch.second);
        } else {
            joined.push_back(stretch);
        }
    }
    for (const auto &[from, to] : joined) {
        KeptSamples::Piece piece;
        piece.start = from;
        piece.samples.reserve(to - from);
        for (const Block &block : m_blocks) {
            const size_t blockEnd = block.start + block.samples.size();
            const size_t lo = std::max(from, block.start);
            const size_t hi = std::min(to, blockEnd);
            if (lo < hi) {
                piece.samples.insert(piece.samples.end(), block.samples.begin() + static_cast<long>(lo - block.start),
                                     block.samples.begin() + static_cast<long>(hi - block.start));
            }
        }
        // Every sample of a stretch to keep was held (letGo() never lets
        // go of one): a short piece would be a bug, and is no piece.
        if (piece.samples.size() == to - from) {
            kept.pieces.push_back(std::move(piece));
        }
    }
    m_blocks.clear();
    m_held = 0;
    return kept;
}

std::optional<std::vector<WindowShift>> windowShifts(std::span<const float> a, std::span<const float> b,
                                                     double rate, double maxShiftSeconds)
{
    return shiftsOver(a.size(), b.size(), wholeSlicer(a), wholeSlicer(b), rate, maxShiftSeconds);
}

std::optional<std::vector<WindowShift>> windowShifts(const KeptSamples &a, const KeptSamples &b, double rate,
                                                     double maxShiftSeconds)
{
    return shiftsOver(a.total, b.total, keptSlicer(a), keptSlicer(b), rate, maxShiftSeconds);
}

std::optional<double> measureShiftMs(const KeptSamples &a, const KeptSamples &b, double rate, double maxShiftSeconds)
{
    return measureOver(a.total, b.total, keptSlicer(a), keptSlicer(b), rate, maxShiftSeconds);
}

bool alignmentCovers(const KeptSamples &a, const KeptSamples &b, double rate)
{
    if (rate <= 0.0) {
        return false;
    }
    for (const Window &w : windowsFor(a.total, b.total, rate)) {
        if (!a.slice(w.startA, w.lengthA) || !b.slice(w.startB, w.lengthB)) {
            return false;
        }
    }
    return true;
}

std::optional<double> measureShiftMs(std::span<const float> a, std::span<const float> b, double rate,
                                     double maxShiftSeconds)
{
    return measureOver(a.size(), b.size(), wholeSlicer(a), wholeSlicer(b), rate, maxShiftSeconds);
}

}  // namespace seabass::domain::alignment
