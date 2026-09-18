// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

namespace seabass::domain
{

// One beat of a track's beat grid, as the DJ software analysed it.
struct Beat
{
    double timeMs = 0.0;
    // 1 to 4, 1 being the downbeat; 0 where the source does not say.
    int beatInBar = 0;
};

// How Engine stores a grid: not every beat, but markers, each pinning one
// beat index to a moment, with the beats between two markers evenly
// spaced. The first marker's index is usually negative and the last one
// past the end of the audio, so that the whole track lies between them.
struct BeatGridMarker
{
    int index = 0;
    double timeMs = 0.0;
};

// Every beat the markers imply that falls inside the track, in order.
// Index 0 is a downbeat and every fourth after it -- Engine's own
// convention. `durationMs` of 0 or less means "unknown": then only beats
// before the track's start are dropped.
std::vector<Beat> beatsFromMarkers(std::vector<BeatGridMarker> markers, double durationMs);

// Where a jump of `beats` whole beats (negative for back) from
// `positionMs` lands: as far into the beat it arrives at as it was into
// the one it left, so the jump stays in time. `beatTimesMs` is the
// grid, in order. Off the grid -- no grid, or before its first beat or
// after its last -- a beat is taken as 625 ms. Never before the start,
// nor within a millisecond of `durationMs` (when that is known), where a
// player would take it for the end of the track.
double positionAfterSkippingBeats(const std::vector<double> &beatTimesMs, double positionMs, int beats,
                                  double durationMs);

}  // namespace seabass::domain
