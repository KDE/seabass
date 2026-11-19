// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "beat_grid_display.hpp"

#include <algorithm>
#include <cmath>
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
    std::vector<BeatTick> inside;
    inside.reserve(beats.size());
    for (const domain::Beat &beat : beats) {
        if (std::isfinite(beat.timeMs) && beat.timeMs >= 0.0 && beat.timeMs <= spanMs) {
            inside.push_back({beat.timeMs, toPx(beat.timeMs), beat.beatInBar == 1});
        }
    }
    if (inside.empty()) {
        return layout;
    }
    if (toPx(smallestGapMs(inside)) >= MinimumBeatSpacingPx) {
        layout.ticks = std::move(inside);
        layout.everyBeat = true;
        layout.barsPerTick = 1;
        return layout;
    }
    std::vector<BeatTick> downbeats;
    for (const BeatTick &tick : inside) {
        if (tick.downbeat) {
            downbeats.push_back(tick);
        }
    }
    if (downbeats.empty()) {
        return layout;
    }
    // N bars span at least N times the smallest bar, so N times it is a
    // spacing every Nth downbeat is sure to keep.
    const double bar = toPx(smallestGapMs(downbeats));
    size_t barsPerTick = 1;
    while (static_cast<double>(barsPerTick) * bar < MinimumDownbeatSpacingPx && barsPerTick < downbeats.size()) {
        barsPerTick *= 2;
    }
    for (size_t k = 0; k < downbeats.size(); k += barsPerTick) {
        layout.ticks.push_back(downbeats[k]);
    }
    layout.barsPerTick = static_cast<int>(barsPerTick);
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
        kept.append(entry);
    }
    return kept;
}

}  // namespace seabass::gui
