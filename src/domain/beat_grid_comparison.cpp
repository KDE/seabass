// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/beat_grid_comparison.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>

namespace seabass::domain
{

namespace
{

std::vector<double> orderedTimes(const std::vector<Beat> &beats, double shiftMs, std::vector<int> *barPlaces)
{
    std::vector<Beat> sorted;
    sorted.reserve(beats.size());
    for (const Beat &beat : beats) {
        if (std::isfinite(beat.timeMs)) {
            sorted.push_back(beat);
        }
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const Beat &a, const Beat &b) { return a.timeMs < b.timeMs; });
    std::vector<double> times;
    times.reserve(sorted.size());
    for (const Beat &beat : sorted) {
        const double time = beat.timeMs + shiftMs;
        // Two beats at one moment would make a beat period of 0, which
        // every ratio below divides by.
        if (!times.empty() && !(time > times.back())) {
            continue;
        }
        times.push_back(time);
        if (barPlaces != nullptr) {
            barPlaces->push_back(beat.beatInBar);
        }
    }
    return times;
}

double medianOf(std::vector<double> values)
{
    if (values.empty()) {
        return 0.0;
    }
    const std::size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle), values.end());
    const double upper = values[middle];
    if (values.size() % 2 == 1) {
        return upper;
    }
    const double lower = *std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle));
    return (lower + upper) / 2.0;
}

struct Line
{
    double slopeMs = 0.0;  // ms per beat
    double maxResidualMs = 0.0;
};

// Least squares through (index, time) for beats first..last, both in.
// The index is centred so that a long grid's sums stay small.
Line fitLine(const std::vector<double> &times, std::size_t first, std::size_t last)
{
    const double count = static_cast<double>(last - first + 1);
    const double meanIndex = (count - 1.0) / 2.0;
    double meanTime = 0.0;
    for (std::size_t i = first; i <= last; ++i) {
        meanTime += times[i];
    }
    meanTime /= count;
    double sxy = 0.0;
    double sxx = 0.0;
    for (std::size_t i = first; i <= last; ++i) {
        const double x = static_cast<double>(i - first) - meanIndex;
        sxy += x * (times[i] - meanTime);
        sxx += x * x;
    }
    Line line;
    line.slopeMs = sxx > 0.0 ? sxy / sxx : 0.0;
    for (std::size_t i = first; i <= last; ++i) {
        const double x = static_cast<double>(i - first) - meanIndex;
        const double residual = std::abs(times[i] - (meanTime + line.slopeMs * x));
        line.maxResidualMs = std::max(line.maxResidualMs, residual);
    }
    return line;
}

GridShape shapeOfTimes(const std::vector<double> &times, double toleranceMs)
{
    GridShape shape;
    shape.beats = times.size();
    if (times.size() < 2) {
        return shape;
    }
    const Line whole = fitLine(times, 0, times.size() - 1);
    shape.maxLineResidualMs = whole.maxResidualMs;
    if (whole.maxResidualMs <= toleranceMs && whole.slopeMs > 0.0) {
        shape.kind = GridShapeKind::Constant;
        shape.bpm = 60000.0 / whole.slopeMs;
        shape.straightRuns = 1;
        shape.lowestBpm = shape.bpm;
        shape.highestBpm = shape.bpm;
        return shape;
    }
    shape.kind = GridShapeKind::Moving;
    // Greedy from the first beat: each run as long as a line still fits
    // it. A run ends on the beat the next one starts on, since a tempo
    // changes at a beat and that beat belongs to both.
    std::size_t start = 0;
    bool first = true;
    while (start + 1 < times.size()) {
        std::size_t end = start + 1;
        Line run = fitLine(times, start, end);
        while (end + 1 < times.size()) {
            const Line longer = fitLine(times, start, end + 1);
            if (longer.maxResidualMs > toleranceMs) {
                break;
            }
            run = longer;
            ++end;
        }
        ++shape.straightRuns;
        if (run.slopeMs > 0.0) {
            const double bpm = 60000.0 / run.slopeMs;
            shape.lowestBpm = first ? bpm : std::min(shape.lowestBpm, bpm);
            shape.highestBpm = first ? bpm : std::max(shape.highestBpm, bpm);
            first = false;
        }
        start = end;
    }
    return shape;
}

// The index of the time in `times` (ordered, not empty) nearest `t`.
std::size_t nearestIndex(const std::vector<double> &times, double t)
{
    const auto above = std::lower_bound(times.begin(), times.end(), t);
    if (above == times.begin()) {
        return 0;
    }
    if (above == times.end()) {
        return times.size() - 1;
    }
    const auto below = std::prev(above);
    return (t - *below) <= (*above - t) ? static_cast<std::size_t>(below - times.begin())
                                         : static_cast<std::size_t>(above - times.begin());
}

TempoRelation classifyRatio(double ratio)
{
    struct Named
    {
        double ratio;
        TempoRelation relation;
    };
    static constexpr std::array<Named, 5> named{{
        {1.0, TempoRelation::Same},
        {0.5, TempoRelation::Half},
        {2.0, TempoRelation::Double},
        {2.0 / 3.0, TempoRelation::TwoThirds},
        {1.5, TempoRelation::ThreeHalves},
    }};
    // Three percent: far wider than any tempo difference two analysers of
    // one track disagree by, far narrower than the four percent between
    // three halves and the next ratio anyone mistakes a tempo for.
    for (const Named &n : named) {
        if (std::abs(std::log(ratio / n.ratio)) < std::log(1.03)) {
            return n.relation;
        }
    }
    return TempoRelation::Other;
}

}  // namespace

const char *tempoRelationName(TempoRelation relation)
{
    switch (relation) {
    case TempoRelation::Unknown:
        return "unknown";
    case TempoRelation::Same:
        return "same";
    case TempoRelation::Half:
        return "half";
    case TempoRelation::Double:
        return "double";
    case TempoRelation::TwoThirds:
        return "two-thirds";
    case TempoRelation::ThreeHalves:
        return "three-halves";
    case TempoRelation::Other:
        return "other";
    }
    return "unknown";
}

const char *gridShapeKindName(GridShapeKind kind)
{
    switch (kind) {
    case GridShapeKind::None:
        return "none";
    case GridShapeKind::Constant:
        return "constant";
    case GridShapeKind::Moving:
        return "moving";
    }
    return "none";
}

GridShape gridShape(const std::vector<Beat> &beats, double toleranceMs)
{
    return shapeOfTimes(orderedTimes(beats, 0.0, nullptr), toleranceMs);
}

BeatGridComparison compareBeatGrids(const std::vector<Beat> &candidate, const std::vector<Beat> &reference,
                                    double candidateShiftMs)
{
    std::vector<int> candidateBars;
    std::vector<int> referenceBars;
    const std::vector<double> c = orderedTimes(candidate, candidateShiftMs, &candidateBars);
    const std::vector<double> r = orderedTimes(reference, 0.0, &referenceBars);

    BeatGridComparison result;
    result.candidateBeats = c.size();
    result.referenceBeats = r.size();
    result.candidateShape = shapeOfTimes(c, GridLineToleranceMs);
    result.referenceShape = shapeOfTimes(r, GridLineToleranceMs);
    if (c.empty() || r.empty()) {
        return result;
    }

    if (r.size() == 1) {
        result.cover = (r.front() >= c.front() && r.front() <= c.back()) ? 1.0 : 0.0;
    } else {
        const double overlap = std::min(c.back(), r.back()) - std::max(c.front(), r.front());
        result.cover = std::clamp(overlap / (r.back() - r.front()), 0.0, 1.0);
    }

    const double halfFirstGap = c.size() > 1 ? (c[1] - c[0]) / 2.0 : 0.0;
    const double halfLastGap = c.size() > 1 ? (c[c.size() - 1] - c[c.size() - 2]) / 2.0 : 0.0;
    const double spanStart = c.front() - halfFirstGap;
    const double spanEnd = c.back() + halfLastGap;

    std::vector<double> offsets;
    std::vector<double> ratios;
    std::array<int, 4> turns{};
    std::size_t hits1 = 0;
    std::size_t hits3 = 0;
    std::size_t hits8 = 0;
    std::size_t hits20 = 0;
    for (std::size_t i = 0; i < r.size(); ++i) {
        if (r[i] < spanStart || r[i] > spanEnd) {
            continue;
        }
        const std::size_t j = nearestIndex(c, r[i]);
        const double offset = c[j] - r[i];
        offsets.push_back(offset);
        const double magnitude = std::abs(offset);
        // A hair over each bound still counts: an offset of exactly 1 ms
        // between two grids comes out of the arithmetic a few ulps either
        // side of it.
        constexpr double Slack = 1e-9;
        hits1 += magnitude <= 1.0 + Slack ? 1 : 0;
        hits3 += magnitude <= 3.0 + Slack ? 1 : 0;
        hits8 += magnitude <= 8.0 + Slack ? 1 : 0;
        hits20 += magnitude <= 20.0 + Slack ? 1 : 0;

        if (r.size() < 2) {
            continue;
        }
        const double referencePeriod = i + 1 < r.size() ? r[i + 1] - r[i] : r[i] - r[i - 1];
        // The candidate's period where this reference beat falls: the gap
        // around it, or the nearest one off either end.
        if (c.size() >= 2) {
            const auto above = std::upper_bound(c.begin(), c.end(), r[i]);
            std::size_t gapEnd = static_cast<std::size_t>(above - c.begin());
            gapEnd = std::clamp<std::size_t>(gapEnd, 1, c.size() - 1);
            const double candidatePeriod = c[gapEnd] - c[gapEnd - 1];
            ratios.push_back(referencePeriod / candidatePeriod);
        }
        if (magnitude <= referencePeriod / 4.0) {
            ++result.matchedBeats;
            const int candidateBar = candidateBars[j];
            const int referenceBar = referenceBars[i];
            if (candidateBar >= 1 && candidateBar <= 4 && referenceBar >= 1 && referenceBar <= 4) {
                ++turns[static_cast<std::size_t>(((candidateBar - referenceBar) % 4 + 4) % 4)];
                ++result.bar.counted;
            }
        }
    }

    const double all = static_cast<double>(r.size());
    result.hits.within1Ms = static_cast<double>(hits1) / all;
    result.hits.within3Ms = static_cast<double>(hits3) / all;
    result.hits.within8Ms = static_cast<double>(hits8) / all;
    result.hits.within20Ms = static_cast<double>(hits20) / all;

    if (!offsets.empty()) {
        OffsetStats &stats = result.offset;
        stats.count = offsets.size();
        stats.medianMs = medianOf(offsets);
        std::vector<double> deviations;
        std::vector<double> magnitudes;
        deviations.reserve(offsets.size());
        magnitudes.reserve(offsets.size());
        for (const double offset : offsets) {
            deviations.push_back(std::abs(offset - stats.medianMs));
            magnitudes.push_back(std::abs(offset));
        }
        stats.madMs = medianOf(deviations);
        std::sort(magnitudes.begin(), magnitudes.end());
        // Nearest rank: the smallest value at least 95 percent of the
        // offsets are no larger than.
        const auto rank = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(magnitudes.size())));
        stats.p95AbsMs = magnitudes[std::max<std::size_t>(rank, 1) - 1];
        stats.maxAbsMs = magnitudes.back();
    }

    if (!ratios.empty()) {
        result.tempoRatio = medianOf(ratios);
        result.tempo = classifyRatio(result.tempoRatio);
    }

    if (result.bar.counted > 0) {
        const auto most = std::max_element(turns.begin(), turns.end());
        result.bar.known = true;
        result.bar.beats = static_cast<int>(most - turns.begin());
        result.bar.share = static_cast<double>(*most) / static_cast<double>(result.bar.counted);
    }
    return result;
}

}  // namespace seabass::domain
