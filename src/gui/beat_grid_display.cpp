// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "beat_grid_display.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

#include <QVariantMap>

namespace seabass::gui
{

QVariantList beatGridForDisplay(const std::vector<domain::Beat> &beats, bool rateRecorded, double shiftMs)
{
    QVariantList grid;
    if (!rateRecorded) {
        return grid;
    }
    grid.reserve(static_cast<qsizetype>(beats.size()));
    for (const domain::Beat &beat : beats) {
        QVariantMap entry;
        entry.insert(QStringLiteral("timeMs"), beat.timeMs + shiftMs);
        entry.insert(QStringLiteral("downbeat"), beat.beatInBar == 1);
        grid.append(entry);
    }
    return grid;
}

namespace
{

// The smallest time between neighbours; infinity for a single beat.
double smallestGapMs(const std::vector<BeatTick> &ticks)
{
    double gap = std::numeric_limits<double>::infinity();
    for (size_t i = 1; i < ticks.size(); ++i) {
        gap = std::min(gap, ticks[i].timeMs - ticks[i - 1].timeMs);
    }
    return gap;
}

// The ticks with the second of every too-close pair taken out: a pair
// less than a tenth of the median gap apart, which a damaged grid has (a
// beat written twice a millisecond apart) and no music does. A section
// at double time or a little over (rekordbox's whole milliseconds give
// 344 against 690) is far above that and keeps every beat. Measured on
// what is left, the tightest place is the music's, and two ticks never
// land on one pixel. Exactly a tenth stays.
std::vector<BeatTick> withoutNearDuplicates(std::vector<BeatTick> ticks)
{
    if (ticks.size() < 3) {
        return ticks;
    }
    std::vector<double> gaps;
    gaps.reserve(ticks.size() - 1);
    for (size_t i = 1; i < ticks.size(); ++i) {
        gaps.push_back(ticks[i].timeMs - ticks[i - 1].timeMs);
    }
    std::nth_element(gaps.begin(), gaps.begin() + static_cast<std::ptrdiff_t>(gaps.size() / 2), gaps.end());
    const double tooClose = gaps[gaps.size() / 2] / 10.0;
    std::vector<BeatTick> kept;
    kept.reserve(ticks.size());
    for (const BeatTick &tick : ticks) {
        if (kept.empty() || tick.timeMs - kept.back().timeMs >= tooClose) {
            kept.push_back(tick);
        }
    }
    return kept;
}

}  // namespace

BeatTickLayout layoutBeatTicks(const std::vector<domain::Beat> &beats, double spanMs, double widthPx)
{
    BeatTickLayout layout;
    if (beats.empty() || !(spanMs > 0.0) || !(widthPx > 0.0) || !std::isfinite(spanMs) || !std::isfinite(widthPx)) {
        return layout;
    }
    // ms * width / span, one rounding each, so a spacing exactly at a
    // threshold is not lost to the arithmetic.
    const auto toPx = [&](double ms) { return ms * widthPx / spanMs; };
    // Every beat of the grid, in time order whatever the source handed
    // over and without the near-duplicates of a damaged grid, before and
    // after the track as well: the bars are counted from the grid's first
    // downbeat, wherever the track starts.
    std::vector<BeatTick> all;
    all.reserve(beats.size());
    for (const domain::Beat &beat : beats) {
        if (std::isfinite(beat.timeMs)) {
            all.push_back({beat.timeMs, toPx(beat.timeMs), beat.beatInBar == 1});
        }
    }
    std::stable_sort(all.begin(), all.end(), [](const BeatTick &a, const BeatTick &b) { return a.timeMs < b.timeMs; });
    all = withoutNearDuplicates(std::move(all));
    // The bars, measured on their own: a downbeat too close to the one
    // before it starts no bar of its own.
    std::vector<BeatTick> bars;
    for (const BeatTick &tick : all) {
        if (tick.downbeat) {
            bars.push_back(tick);
        }
    }
    bars = withoutNearDuplicates(std::move(bars));
    // Each beat's bar: 1 from the first downbeat on, 0 before it.
    {
        size_t next = 0;
        int bar = 0;
        for (BeatTick &tick : all) {
            if (next < bars.size() && tick.downbeat && tick.timeMs == bars[next].timeMs) {
                ++bar;
                bars[next].bar = bar;
                bars[next].startsBar = true;
                tick.startsBar = true;
                ++next;
            }
            tick.bar = bar;
        }
    }
    const auto insideTrack = [&](const BeatTick &tick) { return tick.timeMs >= 0.0 && tick.timeMs <= spanMs; };
    std::vector<BeatTick> inside;
    std::copy_if(all.begin(), all.end(), std::back_inserter(inside), insideTrack);
    if (inside.empty()) {
        return layout;
    }
    std::vector<BeatTick> insideBars;
    std::copy_if(bars.begin(), bars.end(), std::back_inserter(insideBars), insideTrack);
    // The smallest bar in pixels, measured where the track is.
    const double barPx = toPx(smallestGapMs(insideBars));
    // Every `strongBars`th bar is a stronger mark, and carries its number:
    // the smallest power of two from `base` up that keeps the numbers
    // MinimumBarNumberSpacingPx apart.
    const auto strongEvery = [&](size_t base) {
        size_t n = base;
        while (static_cast<double>(n) * barPx < MinimumBarNumberSpacingPx && n < insideBars.size()) {
            n *= 2;
        }
        return n;
    };
    const auto markStrong = [](std::vector<BeatTick> &ticks, size_t every) {
        for (BeatTick &tick : ticks) {
            tick.strong = tick.startsBar && static_cast<size_t>(tick.bar - 1) % every == 0;
        }
    };
    if (toPx(smallestGapMs(inside)) >= MinimumBeatSpacingPx) {
        layout.ticks = std::move(inside);
        layout.everyBeat = true;
        layout.barsPerTick = 1;
        if (!insideBars.empty()) {
            layout.strongBars = static_cast<int>(strongEvery(1));
            markStrong(layout.ticks, static_cast<size_t>(layout.strongBars));
        }
        return layout;
    }
    if (insideBars.empty()) {
        return layout;
    }
    // N bars span at least N times the smallest bar, so N times it is a
    // spacing every Nth downbeat is sure to keep. Every Nth counted from
    // bar 1, so a bar's mark does not move with where the track starts.
    size_t barsPerTick = 1;
    while (static_cast<double>(barsPerTick) * barPx < MinimumDownbeatSpacingPx && barsPerTick < insideBars.size()) {
        barsPerTick *= 2;
    }
    for (const BeatTick &tick : insideBars) {
        if (static_cast<size_t>(tick.bar - 1) % barsPerTick == 0) {
            layout.ticks.push_back(tick);
        }
    }
    layout.barsPerTick = static_cast<int>(barsPerTick);
    layout.strongBars = static_cast<int>(strongEvery(std::max<size_t>(barsPerTick == 1 ? 4 : 16, barsPerTick)));
    markStrong(layout.ticks, static_cast<size_t>(layout.strongBars));
    return layout;
}

QVariantList beatTicksForDisplay(const std::vector<domain::Beat> &beats, bool rateRecorded, double spanMs,
                                 double widthPx)
{
    QVariantList kept;
    if (!rateRecorded) {
        return kept;
    }
    const BeatTickLayout layout = layoutBeatTicks(beats, spanMs, widthPx);
    kept.reserve(static_cast<qsizetype>(layout.ticks.size()));
    for (const BeatTick &tick : layout.ticks) {
        QVariantMap entry;
        entry.insert(QStringLiteral("timeMs"), tick.timeMs);
        entry.insert(QStringLiteral("downbeat"), tick.downbeat);
        entry.insert(QStringLiteral("strong"), tick.strong);
        entry.insert(QStringLiteral("bar"), tick.bar);
        kept.append(entry);
    }
    return kept;
}

}  // namespace seabass::gui
