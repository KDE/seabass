// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

#include "domain/beat_grid.hpp"
#include "domain/waveform.hpp"

namespace seabass::domain
{

// What a player wants of a track's analysis, read in one go: both halves
// live in the same place -- one ANLZ file, one Engine database -- and the
// player loads a track on the GUI thread, now also at every track's end.
// Reading them separately opened and parsed that place twice.
struct TrackAnalysis
{
    std::vector<WaveformColumn> waveform;
    std::vector<Beat> beats;
};

}  // namespace seabass::domain
