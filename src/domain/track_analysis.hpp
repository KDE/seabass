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
    // Whether the beat times rest on a sample rate the library records.
    // rekordbox stores times, so always. Engine stores sample offsets, and
    // a row the player has not analysed records no rate: the reader then
    // works one out from the markers and the tempo, and the same row's
    // cues are at a guess (Track::cueRateUnverified). The player still
    // keeps time by such a grid; nothing draws it as fact.
    bool beatRateRecorded = true;
};

}  // namespace seabass::domain
