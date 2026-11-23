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

struct BeatGridComparison
{
    std::size_t referenceBeats = 0;
    std::size_t candidateBeats = 0;
    // Reference beats whose nearest candidate beat lies within a quarter
    // of the reference's local beat period: the ones the bar and the tempo
    // relation are read from.
    std::size_t matchedBeats = 0;
    OffsetStats offset;
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
