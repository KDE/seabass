// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <span>
#include <string>
#include <vector>

#include "musicanalyze/beat.hpp"

namespace musicanalyze
{

// The tempos an analysis may give out, like the "BPM range" of rekordbox
// and of every DJ program: whether a track is at 87 or at 174 BPM is not
// in the audio (the studies say so, and so do the numbers this was built
// on), it is in what the DJ plays. A tempo found outside the range is taken as its double or
// its half inside it (what is given out can still be a hair outside:
// the ends have half a percent of room, and the later corrections move a
// period by under a percent). A range of one octave (88 to 175) settles
// every such choice; a wider one leaves those inside it to the analyser.
// The default is rekordbox's own, with which its reference grids were
// made.
//
// A range narrower than an octave has no place for some tempos, so it is
// widened at the top to one; rekordbox's own "88-175" is a hair under.
// Ranges are kept between 40 and 240 BPM, which is as slow and as fast
// as the analyser reads a beat; where the top would pass 240 the bottom
// comes down instead.
struct TempoRange
{
    double lowBpm = 70.0;
    double highBpm = 180.0;
};

// The range an analysis works with when given `range`: the same, made
// valid by the rules above. What is no range at all is the default one.
TempoRange usableTempoRange(TempoRange range);

// What the analyser makes of a track: every beat it found, sample 0 of
// the audio at 0 ms, and how sure it is.
struct GridAnalysis
{
    std::vector<Beat> beats;
    // 0 to 1: how much of the track the grid explains.
    double confidence = 0.0;
    // The beats follow a tempo that moves: a list of beats whose spacing
    // changes, where false means one tempo from one place, start to end.
    // A followed grid is the one to look at before trusting it: on music
    // with a steady tempo it is rarely needed.
    bool followsTempo = false;
    // Empty when the analyser ran. Audio it could not work on is reported
    // here rather than as an empty grid, which would read as "no beats in
    // this track" (speech, ambience).
    std::string error;
};

// A followed grid is given out this much earlier than the place the
// onsets peak at. Calibration against rekordbox, whose own two ways of
// analysing differ by as much: measured against the start of the hit
// in the audio on 100 tracks of varied music, its straight grids sit
// 4.9 ms before it at the median and inside the hit on a quarter of
// the tracks, its beat-by-beat grids 6.2 ms before it and inside the
// hit on none. Our straight grids are 2 ms earlier than its straight
// ones as they are; our followed grids were 3.4 ms later than its
// beat-by-beat ones. Early is the safe side: a late grid cuts a
// quantised loop into the kick.
//
// rekordbox's beat-by-beat grids are on whole milliseconds as it
// means them (the section that holds the rest of each millisecond,
// PQT2, is zero for nearly every beat of such a grid), so this stands
// as measured; its straight grids are not, see below.
inline constexpr double FollowedGridEarlierMs = 3.0;

// A straight grid is given out this much later than the place the
// onsets peak at. Calibration against rekordbox's straight grids on
// the tuning half of a collection of 1475 tracks: as measured, ours
// sit 2.2 ms before its grids at the median, tightly (half of the
// tracks between 2.9 and 1.4 ms), so that the 3 ms inside which a
// grid counts as rekordbox's cuts a quarter of them off. 0.7 ms later
// the middle track is 1.5 ms early, nine tracks in ten are still
// early, and no more are late by over 3 ms than before.
//
// Those numbers were against rekordbox's beats in whole
// milliseconds, and its straight grids are not on whole
// milliseconds: PQTZ holds the millisecond and PQT2, in the .EXT,
// the thousandths of it, half a millisecond at the mean. Against its
// beats to the microsecond the same grids were 1.9 ms early at the
// middle and only 547 of 751 within 3 ms; 1.2 ms later puts them
// where the lines above say: 1.4 ms early at the middle, nine in ten
// early, 591 within 3 ms.
inline constexpr double StraightGridLaterMs = 1.2;

// The beat grid of a track.
//
// What it listens to: the audio from 30 Hz to 5 kHz in five bands, the
// envelope of each, and of the envelopes only the rises. Summed, they are
// the onset function, one value a millisecond. What is above 5 kHz is
// left out on purpose: on dance music the hats in it sit between the
// beats, and an onset function that hears them puts the grid there. For
// the place of the beat (not for the tempo) the band under 70 Hz is left
// out too: the bass line plays between the beats as often as on them.
//
// The tempo: the lag at which the onset function repeats, scored together
// with its multiples and a preference for tempos near 128 BPM, sharpened
// on the repeats 4, 16 and 64 beats away,
// each of which says the period that many times finer. That lag and its
// relatives (half, two thirds, three quarters, four thirds, three halves,
// double) are then each tried as the beat: the one at which the track
// folds sharpest is it (window by window, each window free to fold at a
// period of its own within 5 percent, so that a drummer who wanders does
// not blur the comparison), with a wide preference around 128 BPM
// between a tempo and its double. A relative outside the tempo range
// (TempoRange above) counts as its double or half inside it. And in
// doubt the double: where the double of what was chosen lies inside the
// range and the track folds there at least three quarters as sharply, it
// is the tempo (a grid at half the tempo has no lines where the other
// beats are, and more lines help the DJ).
//
// The place of the beat: the onset function folded onto one beat over the
// whole track has most of its weight there (the weight within 30 ms, not
// the highest point: a sharp hit between the beats makes a taller spike
// than a kick, and a kick the heavier hump). Then the track is cut into windows of eight
// beats and each window says how far its own peak is from that place; a
// line through those says how much the period is off and by how much the
// place, and the two are corrected. Windows without a peak near the place
// (a breakdown, a fill) say nothing.
//
// No whole-number rule: the tempo is what was measured.
//
// A tempo that moves: where the straight grid explains fewer than three
// quarters of the windows that lie in a stretch with a rhythm (their own
// peak within 10 ms of it), the track is followed instead. A tempo
// curve is read from it every 2 s, and from the strongest window outwards
// each next window of eight beats corrects the grid when its peak is
// clearly the beat the grid expects; where it is not (a breakdown, a
// fill) the grid goes on at the curve's tempo and the beats in between
// are spaced evenly. Where the curve says the tempo wanders by over a
// percent (a drummer, not a machine) the beats are instead the path
// through the onsets that scores best with steps near the curve's
// period, stretches with nothing to go by spaced evenly. Each followed
// beat is then set on its own onset where it has a clear one within
// 25 ms.
//
// What it does not do yet: say which beat starts the bar (beatInBar is
// 0), or refine the beat on the samples of the transient.
//
// `samples` are mono, full scale 1.0, sample 0 at 0 ms of the beats that
// come back; they are read once and not kept. No samples, or no sample
// rate: the error says so.
GridAnalysis gridFromSamples(std::span<const float> samples, int sampleRateHz, TempoRange range = {});

// A grid's tempo as one number: a minute over the middle gap between two
// beats, not rounded. 0 without two beats. On a grid that follows the
// tempo it is the track's usual tempo, not the tempo everywhere.
double usualBpm(const std::vector<Beat> &beats);

}  // namespace musicanalyze
