// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QVariant>
#include <QVariantMap>

namespace seabass::gui
{

// The beat grid's density rule (layoutBeatTicks(), beat_grid_display.hpp)
// as QML calls it: WaveformView lays out its beatData with layout(), and
// tst_WaveformBeatGrid tests the rule through it. One rule, in C++, so a
// list row handed only the beats it will draw (PlaybackController::
// beatTicksFor()) and a view laying out a whole grid cannot disagree.
class BeatGridTicks : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit BeatGridTicks(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE double minimumBeatSpacing() const;
    Q_INVOKABLE double minimumDownbeatSpacing() const;
    Q_INVOKABLE double minimumBarNumberSpacing() const;

    // `beats`: [{timeMs, downbeat}] in time order, as WaveformView.beatData
    // takes them; anything else (undefined, an entry without a time) gives
    // or contributes no tick. Returns {ticks: [{x, timeMs, downbeat,
    // strong, bar}], everyBeat, barsPerTick, strongBars}, see
    // BeatTickLayout.
    Q_INVOKABLE QVariantMap layout(const QVariant &beats, double spanMs, double widthPx) const;
};

}  // namespace seabass::gui
