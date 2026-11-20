// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "beat_grid_ticks.hpp"

#include <limits>
#include <vector>

#include <QJSValue>
#include <QVariantList>

#include "beat_grid_display.hpp"

namespace seabass::gui
{

double BeatGridTicks::minimumBeatSpacing() const
{
    return MinimumBeatSpacingPx;
}

double BeatGridTicks::minimumDownbeatSpacing() const
{
    return MinimumDownbeatSpacingPx;
}

double BeatGridTicks::minimumBarNumberSpacing() const
{
    return MinimumBarNumberSpacingPx;
}

QVariantMap BeatGridTicks::layout(const QVariant &beats, double spanMs, double widthPx) const
{
    QVariant value = beats;
    if (value.metaType() == QMetaType::fromType<QJSValue>()) {
        value = value.value<QJSValue>().toVariant();
    }
    const QVariantList list = value.toList();
    std::vector<domain::Beat> grid;
    grid.reserve(static_cast<size_t>(list.size()));
    for (const QVariant &entry : list) {
        const QVariantMap beat = entry.toMap();
        const QVariant time = beat.value(QStringLiteral("timeMs"));
        bool ok = false;
        const double timeMs = time.isValid() ? time.toDouble(&ok) : 0.0;
        // An entry without a time is no beat; layoutBeatTicks() drops it.
        grid.push_back({ok ? timeMs : std::numeric_limits<double>::quiet_NaN(),
                        beat.value(QStringLiteral("downbeat")).toBool() ? 1 : 0});
    }
    const BeatTickLayout laidOut = layoutBeatTicks(grid, spanMs, widthPx);
    QVariantList ticks;
    ticks.reserve(static_cast<qsizetype>(laidOut.ticks.size()));
    for (const BeatTick &tick : laidOut.ticks) {
        ticks.append(QVariantMap{{QStringLiteral("x"), tick.x},
                                 {QStringLiteral("timeMs"), tick.timeMs},
                                 {QStringLiteral("downbeat"), tick.downbeat},
                                 {QStringLiteral("strong"), tick.strong},
                                 {QStringLiteral("bar"), tick.bar}});
    }
    return {{QStringLiteral("ticks"), ticks},
            {QStringLiteral("everyBeat"), laidOut.everyBeat},
            {QStringLiteral("barsPerTick"), laidOut.barsPerTick},
            {QStringLiteral("strongBars"), laidOut.strongBars}};
}

}  // namespace seabass::gui
