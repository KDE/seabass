// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "beat_grid_display.hpp"

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

}  // namespace seabass::gui
