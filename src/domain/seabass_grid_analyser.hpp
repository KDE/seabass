// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "domain/grid_analyser.hpp"

namespace seabass::domain
{

// The tempos an analysis may give out, like the "BPM range" of rekordbox
// and of every DJ program: whether a track is at 87 or at 174 BPM is not
// in the audio (the studies say so, and so do our own numbers: see "What
// the literature says" in docs/track-analysis-plan.md), it is in what the
// DJ plays. A tempo found outside the range is taken as its double or
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

// Seabass's own beat grid analyser (docs/track-analysis-plan.md, phase 3).
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
// not blur the comparison), with a wide preference around 128 BPM to
// decide between a tempo and its double. A relative outside the tempo
// range (TempoRange below) counts as its double or half inside it.
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
class SeabassGridAnalyser : public GridAnalyser
{
public:
    explicit SeabassGridAnalyser(TempoRange range = {});

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
    static constexpr double FollowedGridEarlierMs = 3.0;

    // A straight grid is given out this much later than the place the
    // onsets peak at. Calibration against rekordbox's straight grids on
    // the tuning half of a collection of 1475 tracks: as measured, ours
    // sit 2.2 ms before its grids at the median, tightly (half of the
    // tracks between 2.9 and 1.4 ms), so that the 3 ms inside which a
    // grid counts as rekordbox's cuts a quarter of them off. 0.7 ms later
    // the middle track is 1.5 ms early, nine tracks in ten are still
    // early, and no more are late by over 3 ms than before.
    static constexpr double StraightGridLaterMs = 0.7;

    std::string name() const override { return "seabass"; }
    GridAnalysis analyse(PcmSource &source) override;

    // The range in use: the one given, made valid.
    TempoRange tempoRange() const { return m_range; }

private:
    TempoRange m_range;
};

}  // namespace seabass::domain
