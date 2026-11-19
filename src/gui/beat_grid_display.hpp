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
// Every grid a reader gives is drawn. An Engine grid's times rest on the
// rate the row records or, where it records none or libdjinterop cannot
// read it, on the rate the markers themselves give against the tempo;
// where neither gives one, the reader gives no beats at all
// (libdjinterop_beat_grid_reader.cpp). No grid is ever placed by a
// default rate, which is what would make it a guess shown as fact.
QVariantList beatGridForDisplay(const std::vector<domain::Beat> &beats, double shiftMs = 0.0);

// Which beats of a grid get a tick on a waveform this wide: the one
// density rule (#40), decided by pixels, not by which page the waveform is
// on. A whole ten minute track in 600 px is 1300 beats, and drawn every
// one they are a grey smear.
//
//   - Every beat, downbeats marked, when no two neighbouring beats are
//     closer than MinimumBeatSpacingPx.
//   - Otherwise the downbeats alone, when no two are closer than
//     MinimumDownbeatSpacingPx.
//   - Otherwise every Nth downbeat, N the smallest power of two (2, 4, 8,
//     16 bars ...) that keeps them that far apart.
//
// "Closer than" is measured at the tightest place in the track, so a
// tempo change can only ever thin the grid, never crowd part of it; the
// ticks stay where the data puts them. A pair of beats (or of downbeats)
// closer than half the median gap is a damaged grid's, not the music's:
// the second of it is left out, so one bad pair cannot thin the whole
// track nor two ticks share a pixel. Beats come in any order and are laid
// out in time order. A downbeat is a beat with beatInBar 1. Beats before
// the start or after `spanMs` get no tick, and without a length or a
// width there are none.
//
// WaveformView lays out its beatData with this (BeatGridTicks, the QML
// face of it), and a list row is handed only the beats this keeps
// (PlaybackController::beatTicksFor()). Laid out again, those come out as
// the same ticks, so the two never disagree.
inline constexpr double MinimumBeatSpacingPx = 6.0;
inline constexpr double MinimumDownbeatSpacingPx = 12.0;

struct BeatTick
{
    double timeMs = 0.0;
    double x = 0.0;  // pixels from the left edge
    bool downbeat = false;
};

struct BeatTickLayout
{
    std::vector<BeatTick> ticks;
    bool everyBeat = false;  // whether the ordinary beats are drawn
    int barsPerTick = 0;     // the N of "every Nth downbeat"; 1 for every one (or every beat), 0 for none
};

BeatTickLayout layoutBeatTicks(const std::vector<domain::Beat> &beats, double spanMs, double widthPx);

// The beats layoutBeatTicks() keeps, as beatGridForDisplay() hands them.
QVariantList beatTicksForDisplay(const std::vector<domain::Beat> &beats, double spanMs, double widthPx);

}  // namespace seabass::gui
