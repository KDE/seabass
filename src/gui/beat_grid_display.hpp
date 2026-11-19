// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

#include <QVariantList>

#include "domain/beat_grid.hpp"

namespace seabass::gui
{

// A track's beat grid as WaveformView's beatData takes it (#40): one
// {timeMs, downbeat} per beat, in order, each time moved by `shiftMs`
// (the player's timeline shift, see playback_timeline.hpp; 0 elsewhere).
// downbeat is true for the first beat of a bar and false for every other
// beat, including one whose place in the bar the library does not say.
//
// Empty when `rateRecorded` is false (domain::TrackAnalysis::
// beatRateRecorded): a grid placed by a sample rate the library does not
// record is not drawn at all. A wrong grid shown as fact is worse than
// none.
QVariantList beatGridForDisplay(const std::vector<domain::Beat> &beats, bool rateRecorded, double shiftMs = 0.0);

}  // namespace seabass::gui
