// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "domain/track.hpp"

// Analyse again gives a track a new beat grid. Cues and loops the DJ put
// on a beat of the old grid belong to that beat, not to the millisecond,
// so they can follow the grid to the beat of the same number. Never
// without the person's word: this plans what would move, and the caller
// moves only what the person agreed to.
namespace seabass::application
{

// A cue lies on a beat of the old grid when it is this close to one.
inline constexpr double CueOnBeatToleranceMs = 5.0;
// A move shorter than this is no move: the cue is left alone.
inline constexpr double CueMoveMinimumMs = 1.0;

// One cue (or loop) that would move, as the review shows it.
struct CueGridMove
{
    // Its place in the cue list the plan was made from.
    std::size_t cueIndex = 0;
    domain::CuePoint::Kind kind = domain::CuePoint::Kind::Memory;
    int hotCueNumber = 0;
    bool isLoop = false;
    std::string color;
    std::string comment;
    double oldMs = 0.0;
    double newMs = 0.0;
    // The beat both positions are on, counted from the grids' first (0).
    std::size_t beat = 0;
    // A loop's end, before and after; its beat when the end was on one
    // too (and then follows that beat), none when it moves by the start's
    // amount.
    double oldEndMs = 0.0;
    double newEndMs = 0.0;
    std::optional<std::size_t> endBeat;
};

struct CueGridPlan
{
    enum class Verdict {
        // The old grid has fewer than two beats: nothing can be on it.
        NoOldGrid,
        // No cue would move (none on a beat, or none by a millisecond).
        NothingMoves,
        // `moves` would move.
        Moves,
        // A cue on a beat of the old grid has no beat of the same number
        // within a quarter of a beat in the new one (another tempo, a
        // grid started elsewhere, a shorter grid): numbering beats is not
        // to be trusted between these two grids, so nothing moves.
        GridChangedTooMuch,
    };
    Verdict verdict = Verdict::NoOldGrid;
    std::vector<CueGridMove> moves;
    // Cues not on a beat of the old grid: they stay where they are.
    int offBeat = 0;
    // Cues on a beat whose move would be under CueMoveMinimumMs.
    int alreadyThere = 0;
};

// What would move, from `cues` and the two grids (beat times in ms, in
// order, all on one timeline: rekordbox's, where every library's cues are
// read). Per cue:
// - on a beat when within CueOnBeatToleranceMs of one of the old grid;
//   the nearest such beat is its beat;
// - its new place is the new grid's beat of the same number, the cue's
//   offset from its old beat kept;
// - a loop keeps its length in beats: its end follows its own beat when
//   it was on one, else moves by the start's amount;
// - trusted only when the new beat is within a quarter of the old grid's
//   beat spacing there (and so is a loop end's); one untrusted cue makes
//   the track GridChangedTooMuch and no cue moves;
// - a start that would come out before 0 lands on 0, the track's start; a
//   loop whose start lands there keeps its length, its end moving by what
//   the start moved (and then has no endBeat). Every move in the plan, and
//   every amount the review shows and the threshold compares, is the move
//   as it is made.
CueGridPlan planCueGridMoves(const std::vector<domain::CuePoint> &cues, const std::vector<double> &oldBeats,
                             const std::vector<double> &newBeats);

// One move from `oldMs` to `newMs` as an amount (earlier and later
// alike), and as it is stored: rekordbox's analysis files keep whole ms,
// each position cut to the whole ms below it, so a move of 9.3 ms from
// 1000.0 to 990.7 is stored as 1000 to 990, 10 ms. The larger of the two.
double cueMoveMs(double oldMs, double newMs);

// The largest move of the plan (cueMoveMs()): of every move's start and
// of a loop's end. 0 for a plan that moves nothing.
double largestCueMoveMs(const CueGridPlan &plan);

// Whether the track's cues follow the new grid without the person being
// asked: the plan moves, and its largest move is under `thresholdMs`. One
// answer for the whole track, as its cues move all or none: a track with
// one move at or over the threshold is asked about every move. A
// threshold of 0 asks about every track.
bool cuesFollowOnTheirOwn(const CueGridPlan &plan, double thresholdMs);

// `cues` with the plan's moves made; nothing else of any cue changes. The
// plan must have been made from these cues: an index out of range throws
// std::out_of_range.
std::vector<domain::CuePoint> cuesMovedOntoNewGrid(std::vector<domain::CuePoint> cues, const CueGridPlan &plan);

}  // namespace seabass::application
