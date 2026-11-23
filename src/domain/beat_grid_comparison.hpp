// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <vector>

#include "domain/beat_grid.hpp"

// How far one beat grid is from another, beat by beat, so that a grid
// whose tempo moves is measured the same way as a constant one
// (docs/track-analysis-plan.md, "The grid comparison"). Both grids must
// be on one timeline before they are compared: the caller puts them
// there (a rekordbox grid with the lead-in code, say) and may add a
// further shift here.
namespace seabass::domain
{

// The candidate's tempo against the reference's, from the ratio of their
// local beat periods. Half: the candidate counts half as many beats (its
// period is twice the reference's). TwoThirds and ThreeHalves likewise
// name the candidate's tempo as a share of the reference's.
enum class TempoRelation { Unknown, Same, Half, Double, TwoThirds, ThreeHalves, Other };

const char *tempoRelationName(TempoRelation relation);

enum class GridShapeKind {
    // Fewer than two beats: nothing to fit.
    None,
    // One straight line through beat index and time leaves no beat
    // further from it than the tolerance.
    Constant,
    Moving,
};

const char *gridShapeKindName(GridShapeKind kind);

struct GridShape
{
    GridShapeKind kind = GridShapeKind::None;
    std::size_t beats = 0;
    // The slope of the line through every beat, as a tempo. Set for a
    // constant grid only.
    double bpm = 0.0;
    // The furthest any beat lies from that line, in ms.
    double maxLineResidualMs = 0.0;
    // Straight runs: the grid cut, from the first beat on, into the
    // longest stretches a line fits within the tolerance. 1 for a
    // constant grid. A grid that ramps is cut into many short runs; one
    // that steps once into two.
    int straightRuns = 0;
    // The slowest and fastest run, as a tempo (each run's slope). The
    // same as bpm on a constant grid.
    double lowestBpm = 0.0;
    double highestBpm = 0.0;
};

// rekordbox stores whole milliseconds, so a line through its beats is up
// to half a millisecond off each of them; the tolerance leaves room for
// that and for a sample-rounded Engine grid on top.
inline constexpr double GridLineToleranceMs = 1.5;

GridShape gridShape(const std::vector<Beat> &beats, double toleranceMs = GridLineToleranceMs);

// Each reference beat against the nearest candidate beat, candidate minus
// reference, in ms. Only reference beats inside the candidate's span (half
// a beat either side of its first and last beat) take part: past its ends
// the candidate has nothing to be near.
struct OffsetStats
{
    std::size_t count = 0;
    // The systematic shift.
    double medianMs = 0.0;
    // The median absolute deviation around the median: the scatter.
    double madMs = 0.0;
    // Of the absolute offset.
    double p95AbsMs = 0.0;
    double maxAbsMs = 0.0;
};

// Shares of ALL reference beats (those outside the candidate's span count
// as misses) whose nearest candidate beat is at most this far away.
struct HitRates
{
    double within1Ms = 0.0;
    double within3Ms = 0.0;
    double within8Ms = 0.0;
    double within20Ms = 0.0;
};

// By how many beats the candidate's bar is turned against the
// reference's: (candidate beatInBar - reference beatInBar) mod 4 over the
// matched beats where both say, the most common value, and the share of
// those beats that have it.
struct BarTurn
{
    bool known = false;
    int beats = 0;
    double share = 0.0;
    std::size_t counted = 0;
};

// How far the candidate runs away from the reference over the track, in
// ms: what a tempo that is a little off adds up to. 0.01 BPM at 128 is
// 28 ms after six minutes, on a grid whose first beats are exact, and it
// shows here and nowhere else.
//
// The offset is followed from beat to beat, in stretches. A reference
// beat goes on a stretch when the candidate beat nearest the place the
// stretch's last offset puts it is within a quarter of a beat (of the
// reference's beat there); so an offset that grows past half a beat is
// followed and not wrapped back. A reference beat that does not go on is
// left out: one in a hole of the candidate (a tracker that gave no beats
// for a while, a stretch at half the tempo), one with a stray beat, the
// ones of a short while on the off-beat. The stretch goes on after them
// where the candidate is back within a quarter of a beat of its last
// offset. Drift across what was left out counts as long as it stays
// under that quarter.
//
// A new stretch begins at a reference beat the candidate has a beat
// within half a beat of, begins again at the next such beat that does
// not go on it, and takes the place of the stretch before it once it has
// eight beats: the candidate has then moved for good (onto the off-beat,
// or back from a long while there), which is a jump and not drift. A
// candidate that moves on by a whole beat at once is not seen to move:
// the stretch finds a beat at its place and goes on.
//
// The drift is the slope of the longest stretch, over the time from the
// first followed beat of the track to the last. The slope is that of the
// line through the median offset of the stretch's first fifth and that
// of its last, each at the median time of its beats, so a few beats
// thrown off move it by a beat's worth of drift at most. Where the
// longest stretch is not the whole track, this takes the rest to run at
// the tempo it has. A step inside a stretch (the candidate sliding on by
// a part of a beat, a quarter at a time or less, and staying there) is
// not a line: it comes out a quarter larger than it is when the stretch
// is the whole track, and larger still when it is not.
//
// Known only where the two have the same tempo and the longest stretch
// has at least ten beats and at least half of all the beats followed: the
// slope of a small part of a track says little about the whole. The
// reference is taken to have every beat: a hole in the reference is
// taken for one long beat.
struct Drift
{
    bool known = false;
    double ms = 0.0;
    // The longest stretch's line at the first and at the last followed
    // beat of the track: the candidate's offset there where that stretch
    // reaches it.
    double startMs = 0.0;
    double endMs = 0.0;
};

struct BeatGridComparison
{
    std::size_t referenceBeats = 0;
    std::size_t candidateBeats = 0;
    // Reference beats whose nearest candidate beat lies within a quarter
    // of the reference's local beat period: the ones the bar and the tempo
    // relation are read from.
    std::size_t matchedBeats = 0;
    OffsetStats offset;
    Drift drift;
    HitRates hits;
    TempoRelation tempo = TempoRelation::Unknown;
    // The median of candidate tempo over reference tempo, beat by beat.
    // 0 when unknown.
    double tempoRatio = 0.0;
    BarTurn bar;
    GridShape candidateShape;
    GridShape referenceShape;
    // The share of the reference's span (first to last beat) the
    // candidate's span covers, 0 to 1.
    double cover = 0.0;
};

// `candidateShiftMs` is added to every candidate beat before anything is
// measured. Both grids are taken in time order, without beats that are
// not a number or that do not come after the one before them. Beats
// before 0 are kept: a shifted grid may start there.
BeatGridComparison compareBeatGrids(const std::vector<Beat> &candidate, const std::vector<Beat> &reference,
                                    double candidateShiftMs = 0.0);

}  // namespace seabass::domain
