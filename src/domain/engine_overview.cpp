// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/engine_overview.hpp"

#include <algorithm>
#include <cmath>

namespace seabass::domain
{

namespace
{

// Where every number below comes from: 192 tracks of one real library
// that both rekordbox (7.2) and a Denon player had analysed, paired by
// file path. Each fit was made on half of them (even Engine ids) and
// tested on the other half; on those, the overview from PWV6 is off by a
// median of about 9 per point on the 0..255 scale (90th percentile
// about 14). They are measurements of how two analysers happen to
// agree, not constants of either format: another rekordbox version, or
// another player firmware, may need them measured again.

// overview[L, M, H] = [b0, b1, b2, 1] . W, rows b0, b1, b2, constant.
constexpr double ThreeBandWeights[4][3] = {
    {4.815622842289163, 0.6040735232915603, 0.058422590664549676},
    {-0.748213085635203, 1.8067746248085388, 0.09499987609770366},
    {0.034566486148260606, -0.03575619957126411, 0.5964397504995116},
    {3.79629329084852, 6.74169331099033, -2.002199610127076},
};

// overview[L, M, H] = [c0 .. c5, 1] . W from the six bytes of a PWV4
// entry, fitted without the half-entry shift the PWV6 fit uses. Test
// half: median error about 12, against about 9 from PWV6.
constexpr double ColourWeights[7][3] = {
    {-0.11919115162312342, 0.27795094261404824, 0.24534218639731328},
    {-0.025689534679386283, -0.010470135585769869, -0.036990842570589086},
    {0.4713039621847427, 0.4846092566369064, -0.14534370026411997},
    {1.2267407790115004, -0.4943142760442573, -0.06422821159176771},
    {-0.05745990001417988, 0.49479423047086096, 0.02135854026069438},
    {0.1765817630631859, 0.12803846547622844, 0.22352457208773804},
    {2.281951467545576, 5.1407911182705215, 4.8209608279789515},
};

// The .rgb file's points from the player's own overview, rows overview
// L, M, H, constant; columns the file's three bytes. The fit was made
// against the player's overview, not a converted one, and only these
// three-decimal values were recorded. Test half: median error about 5
// per point when fed the player's overview; fed a converted overview the
// overview's own error comes on top.
constexpr double SecondWaveformWeights[4][3] = {
    {0.597, -0.084, 0.013},
    {0.203, 0.614, -0.024},
    {-0.467, 0.388, 1.471},
    {2.397, 1.914, 2.169},
};

// Loudness = a * (overview band mean / 255) + c, per band. The fuller
// fit the measurement tried (25 features of the overview) was barely
// better and its weights were never written down; this one was fitted
// on 348 player-analysed tracks of the same library, even ids, and on
// the odd ones is off by a mean of 0.078, 0.044 and 0.037 (low, mid,
// high), against 0.098, 0.062 and 0.058 for just guessing the mean.
constexpr double LoudnessSlope[3] = {0.60555172422179, 0.8817207162380182, 2.3108329350136936};
constexpr double LoudnessOffset[3] = {0.4180991910089868, 0.20371712901360284, 0.08844842823637834};

// rekordbox's scrolling waveforms have 150 entries a second.
constexpr double ScrollEntriesPerSecond = 150.0;

// An MPEG-1 Layer III frame decodes to 1152 samples.
constexpr std::uint64_t Mp3FrameSamples = 1152;

// numpy rounds halves to even, and the fit and the reference outputs were
// made with it; nearbyint in the default rounding mode does the same.
std::uint8_t toByte(double value)
{
    return static_cast<std::uint8_t>(std::clamp(std::nearbyint(value), 0.0, 255.0));
}

// The source's entries, spread evenly over `sourceSeconds`, averaged into
// PointCount points spread evenly over `targetSeconds`: each point the
// mean of the entries it overlaps, weighted by how much of each it
// overlaps. The durations are not stretched to match: rekordbox and the
// player count a track's length slightly differently, and matching
// times, not proportions, is what lined the two up best. A point past
// the end of the source is all zeros. `shiftEntries` moves the source
// earlier by that many entries.
std::vector<std::vector<double>> resampled(std::string_view entries, std::size_t entryBytes, double sourceSeconds,
                                           double targetSeconds, double shiftEntries)
{
    const std::size_t count = entries.size() / entryBytes;
    const std::size_t n = EngineOverview::PointCount;
    const double entrySeconds = sourceSeconds / static_cast<double>(count);
    const double offset = shiftEntries * sourceSeconds / static_cast<double>(count);
    std::vector<std::vector<double>> out(n, std::vector<double>(entryBytes, 0.0));
    for (std::size_t j = 0; j < n; ++j) {
        // Written in the order the measuring script computed it, so the
        // two agree to the last bit wherever they can.
        const double i0 = (static_cast<double>(j) * targetSeconds / static_cast<double>(n) + offset) / entrySeconds;
        const double i1 = (static_cast<double>(j + 1) * targetSeconds / static_cast<double>(n) + offset) / entrySeconds;
        const auto first = static_cast<std::ptrdiff_t>(std::max(0.0, std::floor(i0)));
        const auto last = static_cast<std::ptrdiff_t>(std::min(static_cast<double>(count), std::ceil(i1)));
        std::vector<double> weights;
        double total = 0.0;
        for (std::ptrdiff_t i = first; i < last; ++i) {
            const double w = std::max(0.0, std::min(static_cast<double>(i + 1), i1) - std::max(static_cast<double>(i), i0));
            weights.push_back(w);
            total += w;
        }
        if (total <= 0.0) {
            continue;
        }
        for (std::size_t k = 0; k < weights.size(); ++k) {
            const double w = weights[k] / total;
            const std::size_t at = static_cast<std::size_t>(first + static_cast<std::ptrdiff_t>(k)) * entryBytes;
            for (std::size_t b = 0; b < entryBytes; ++b) {
                out[j][b] += w * static_cast<unsigned char>(entries[at + b]);
            }
        }
    }
    return out;
}

template<std::size_t Rows>
BandTriple applied(const std::vector<double> &inputs, const double (&weights)[Rows][3])
{
    double result[3];
    for (std::size_t c = 0; c < 3; ++c) {
        double sum = 0.0;
        for (std::size_t r = 0; r + 1 < Rows; ++r) {
            sum += inputs[r] * weights[r][c];
        }
        result[c] = sum + weights[Rows - 1][c];
    }
    return {toByte(result[0]), toByte(result[1]), toByte(result[2])};
}

template<std::size_t Rows>
EngineOverview converted(std::string_view entries, std::size_t entryBytes, double sourceSeconds, double targetSeconds,
                         double shiftEntries, const double (&weights)[Rows][3])
{
    if (entries.empty() || entries.size() % entryBytes != 0 || !(sourceSeconds > 0.0) || !(targetSeconds > 0.0)
        || !std::isfinite(sourceSeconds) || !std::isfinite(targetSeconds)) {
        return {};
    }
    EngineOverview overview;
    overview.points.reserve(EngineOverview::PointCount);
    for (const auto &point : resampled(entries, entryBytes, sourceSeconds, targetSeconds, shiftEntries)) {
        overview.points.push_back(applied(point, weights));
    }
    overview.finalTriple = perBandMaximum(overview.points);
    return overview;
}

}  // namespace

BandTriple perBandMaximum(const std::vector<BandTriple> &points)
{
    BandTriple maximum;
    for (const auto &point : points) {
        maximum.low = std::max(maximum.low, point.low);
        maximum.mid = std::max(maximum.mid, point.mid);
        maximum.high = std::max(maximum.high, point.high);
    }
    return maximum;
}

EngineOverview overviewFromThreeBandPreview(std::string_view entries, double sourceSeconds, double targetSeconds)
{
    // Shifted by half an entry: the best of the shifts tried, by a small
    // margin (median error 9.09 against 9.53 unshifted).
    return converted(entries, 3, sourceSeconds, targetSeconds, 0.5, ThreeBandWeights);
}

EngineOverview overviewFromColourPreview(std::string_view entries, double sourceSeconds, double targetSeconds)
{
    return converted(entries, 6, sourceSeconds, targetSeconds, 0.0, ColourWeights);
}

EngineOverview secondWaveformFromOverview(const EngineOverview &overview)
{
    if (overview.empty()) {
        return {};
    }
    EngineOverview second;
    second.points.reserve(overview.points.size());
    for (const auto &point : overview.points) {
        second.points.push_back(applied(std::vector<double>{static_cast<double>(point.low),
                                                            static_cast<double>(point.mid),
                                                            static_cast<double>(point.high)},
                                        SecondWaveformWeights));
    }
    // What the player puts in this file's last triple was not worked out.
    // It is not the maximum of the file's own points (it was below it on
    // all five reference tracks); the per-band maximum stands in because
    // that is what the overview's last triple is, until it is measured.
    second.finalTriple = perBandMaximum(second.points);
    return second;
}

std::uint64_t engineSampleCount(std::uint32_t scrollEntries, double sampleRateHz, bool isMp3)
{
    if (scrollEntries == 0 || !(sampleRateHz > 0.0) || !std::isfinite(sampleRateHz)) {
        return 0;
    }
    // Measured at 44.1 kHz only (294 samples an entry); written for any
    // rate on the assumption that rekordbox keeps 150 entries a second.
    const double perEntry = sampleRateHz / ScrollEntriesPerSecond;
    const double upper = static_cast<double>(scrollEntries) * perEntry;
    const double lower = upper - perEntry;
    // Every other file: the player's count lies within about 3 ms of the
    // middle of the last entry.
    const double middle = upper - perEntry / 2.0;
    if (!isMp3) {
        return static_cast<std::uint64_t>(std::llround(middle));
    }
    // The player counts whole MP3 frames, and rekordbox's entry count is
    // that count divided by the samples per entry, rounded up: so the
    // count is the multiple of 1152 in (lower, upper]. An entry is
    // narrower than a frame, so there is at most one; where there is
    // none, the entry count did not come from a whole number of frames,
    // and the nearest multiple to the middle of the last entry is the
    // best guess left.
    const auto firstAbove = static_cast<std::uint64_t>(std::floor(lower / Mp3FrameSamples)) + 1;
    const std::uint64_t candidate = firstAbove * Mp3FrameSamples;
    if (static_cast<double>(candidate) <= upper) {
        return candidate;
    }
    return static_cast<std::uint64_t>(std::llround(middle / Mp3FrameSamples)) * Mp3FrameSamples;
}

std::array<double, 3> loudnessFromOverview(const EngineOverview &overview)
{
    if (overview.empty()) {
        return {0.0, 0.0, 0.0};
    }
    double sum[3] = {0.0, 0.0, 0.0};
    for (const auto &point : overview.points) {
        sum[0] += point.low;
        sum[1] += point.mid;
        sum[2] += point.high;
    }
    std::array<double, 3> loudness{};
    for (std::size_t b = 0; b < 3; ++b) {
        const double mean = sum[b] / static_cast<double>(overview.points.size()) / 255.0;
        loudness[b] = std::max(0.0, LoudnessSlope[b] * mean + LoudnessOffset[b]);
    }
    return loudness;
}

}  // namespace seabass::domain
