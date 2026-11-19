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

// A grid of every beat, as rekordbox stores it, put the way Engine stores
// one: markers, one wherever the tempo changes and one at each end. The
// inverse of beatsFromMarkers(): fed back through it, every source beat
// comes back within a millisecond, with the same place in the bar.
struct MarkersFromBeats
{
    // Empty: no grid could be made.
    std::vector<BeatGridMarker> markers;
    // Places where the source's bar count does not continue (beat 3
    // followed by beat 1, say). Engine counts bars from index 0 in fours
    // and cannot say that; the first bar position wins.
    int barBreaks = 0;
};

// `durationMs` of 0 or less means "unknown": then the last marker stays
// on the last beat instead of reaching past the end of the track.
MarkersFromBeats markersFromBeats(const std::vector<Beat> &beats, double durationMs);

// Markers as markersFromBeats() gives them, numbered the way Engine
// numbers its own: the first marker is beat -4, before the track starts,
// and beat 0 is the first downbeat at or after the start, or within a
// millisecond before it (the line through rounded beats is that far
// off). The first
// marker moves along its span's line to get there, and every index moves
// by the same multiple of four, so each beat keeps its place in the bar.
// Markers that cannot be put so (fewer than two, a number that is not
// one, a tempo change inside the first bar) come back as they were.
std::vector<BeatGridMarker> engineNumbered(std::vector<BeatGridMarker> markers);

// A grid as read from a file, made fit to keep time by: every beat after
// the one before it, none before the track starts, none that is not a
// number. Whatever breaks that is dropped. Two beats at one moment would
// otherwise be a beat of no length, and a display dividing by it.
std::vector<Beat> beatsInOrder(std::vector<Beat> beats);

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
