// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Analyse again's save compares each library's own plan for a track's
// cues with the plan the review showed
// (gui/edit/changes/reanalyse_track_change.hpp: cuesMatchTheReview(),
// engineCuesOntoNewGrid()). rekordbox keeps whole ms and Engine samples,
// so near the track's start the hold at 0 meets each library's copy of a
// cue differently. The grid moves by -2.2 ms, its first beat at 2.2 ms:
//
// 1. An intro loop at 2 ms in rekordbox, 2.4 ms in Engine: rekordbox's
//    start is held at 0, Engine's is not; both are the same loop, and
//    Engine's moves.
// 2. A cue at 1.1 ms in one library and 0.8 ms in the other: one a move
//    of 1.1 ms, the other under the minimum; the same cue, either way
//    round, in rekordbox's comparison and in Engine's.
// 3. A cue on another beat, or another pad, is still another cue.
// 4. Engine's main cue follows a memory cue by the grid's move at its
//    beat and is never put before the track's start.

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "application/use_cases/carry_cues_to_new_grid.hpp"
#include "domain/cue_tolerance.hpp"
#include "gui/edit/changes/reanalyse_track_change.hpp"

using namespace seabass;
using application::CueGridPlan;
using gui::ReanalyseTrackChange;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

std::vector<double> grid(double first, double spacing, int count)
{
    std::vector<double> beats;
    for (int i = 0; i < count; ++i) {
        beats.push_back(first + i * spacing);
    }
    return beats;
}

domain::CuePoint hot(int pad, double ms)
{
    domain::CuePoint cue;
    cue.kind = domain::CuePoint::Kind::Hot;
    cue.hotCueNumber = pad;
    cue.positionMs = ms;
    return cue;
}

domain::CuePoint hotLoop(int pad, double from, double to)
{
    domain::CuePoint cue = hot(pad, from);
    cue.isLoop = true;
    cue.loopEndMs = to;
    return cue;
}

domain::CuePoint memory(double ms)
{
    domain::CuePoint cue;
    cue.positionMs = ms;
    return cue;
}

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-6;
}

}  // namespace

int main()
{
    const std::vector<double> oldBeats = grid(2.2, 500.0, 40);
    const std::vector<double> newBeats = grid(0.0, 500.0, 40);
    const double tolerance = domain::cueToleranceMsFor(120.0, 120.0);
    const auto plan = [&](const std::vector<domain::CuePoint> &cues) {
        return application::planCueGridMoves(cues, oldBeats, newBeats);
    };

    // 1.
    {
        const std::vector<domain::CuePoint> rekordbox{hotLoop(1, 2.0, 2002.0)};
        const std::vector<domain::CuePoint> engine{hotLoop(1, 2.4, 2002.4)};
        const CueGridPlan review = plan(rekordbox);
        check(review.moves.size() == 1, "the review moves the intro loop");
        check(ReanalyseTrackChange::cuesMatchTheReview(review, plan(rekordbox), tolerance),
              "rekordbox's own plan is the review's");
        check(ReanalyseTrackChange::cuesMatchTheReview(plan(engine), plan(rekordbox), tolerance),
              "the loop held at 0 and the one not held are one loop");
        check(ReanalyseTrackChange::cuesMatchTheReview(review, plan(engine), tolerance),
              "and the other way round");
        const auto moved = ReanalyseTrackChange::engineCuesOntoNewGrid(review, engine, oldBeats, newBeats, tolerance);
        check(moved.has_value(), "Engine's loop is the review's");
        if (moved) {
            check(near(moved->at(0).positionMs, 0.2) && near(moved->at(0).loopEndMs, 2000.2),
                  "Engine's loop moves by the grid's -2.2 ms, its end on its beat, got "
                      + std::to_string(moved->at(0).positionMs) + " to " + std::to_string(moved->at(0).loopEndMs));
        }
        std::cout << "case 1 (an intro loop at 2 ms and 2.4 ms: one loop, and Engine's moves) done\n";
    }

    // 2.
    {
        for (const bool rekordboxMoves : {true, false}) {
            const std::string way = rekordboxMoves ? "1.1 ms in the review, 0.8 ms in the library"
                                                   : "0.8 ms in the review, 1.1 ms in the library";
            const double shown = rekordboxMoves ? 1.1 : 0.8;
            const double held = rekordboxMoves ? 0.8 : 1.1;
            const CueGridPlan review = plan({hot(2, shown), hotLoop(1, 2.0, 2002.0)});
            check(ReanalyseTrackChange::cuesMatchTheReview(review, plan({hot(2, held), hotLoop(1, 2.0, 2002.0)}),
                                                           tolerance),
                  way + ": rekordbox's comparison takes them for one cue");
            check(ReanalyseTrackChange::cuesMatchTheReview(review, plan({memory(held), hotLoop(1, 2.0, 2002.0)}),
                                                           tolerance)
                      == false,
                  way + ": a memory cue is not a hot cue");
            const auto moved = ReanalyseTrackChange::engineCuesOntoNewGrid(
                review, {hot(2, held), hotLoop(1, 2.4, 2002.4)}, oldBeats, newBeats, tolerance);
            check(moved.has_value(), way + ": Engine's comparison takes them for one cue");
            if (moved) {
                check(moved->at(0).positionMs >= 0.0 && moved->at(0).positionMs < 1.0,
                      way + ": Engine's cue is at the start or within a millisecond of it");
                check(near(moved->at(1).positionMs, 0.2), way + ": and its loop moves");
            }
            // Memory cues, as rekordbox holds them.
            check(ReanalyseTrackChange::cuesMatchTheReview(plan({memory(shown)}), plan({memory(held)}), tolerance),
                  way + ": a memory cue the same way");
        }
        std::cout << "case 2 (a cue at 1.1 ms and its copy at 0.8 ms: one cue, either way round) done\n";
    }

    // 3.
    {
        const CueGridPlan review = plan({hot(2, 1.1)});
        check(!ReanalyseTrackChange::cuesMatchTheReview(review, plan({hot(3, 0.8)}), tolerance),
              "another pad is another cue");
        check(!ReanalyseTrackChange::engineCuesOntoNewGrid(review, {hot(3, 0.8)}, oldBeats, newBeats, tolerance),
              "Engine's cue on another pad is another cue");
        check(!ReanalyseTrackChange::cuesMatchTheReview(review, plan({hot(2, 502.0)}), tolerance),
              "a cue on another beat is another cue");
        check(!ReanalyseTrackChange::cuesMatchTheReview(review, plan({}), tolerance), "no cue is not the review's cue");
        check(!ReanalyseTrackChange::cuesMatchTheReview(plan({}), review, tolerance),
              "a library moving a cue the review does not hold is not the review");
        // A loop whose end is on a beat in one library and not in the
        // other is another loop.
        check(!ReanalyseTrackChange::cuesMatchTheReview(plan({hotLoop(1, 2.0, 2002.0)}), plan({hotLoop(1, 2.0, 1802.0)}),
                                                        tolerance),
              "a loop ending on another beat is another loop");
        std::cout << "case 3 (another pad, another beat, no cue: another cue) done\n";
    }

    // 4.
    {
        // The review moves a memory cue from 3.0 ms by the grid's -2.2 ms
        // (not held); Engine's main cue at 1.5 ms is that cue, and goes
        // to the start, not to -0.7 ms.
        const CueGridPlan review = plan({memory(3.0)});
        check(review.moves.size() == 1 && near(review.moves[0].newMs, 0.8), "the review moves the memory cue");
        const auto moved =
            ReanalyseTrackChange::engineCuesOntoNewGrid(review, {memory(1.5), hot(1, 2.4)}, oldBeats, newBeats, tolerance);
        check(moved.has_value(), "Engine's cues are the review's");
        if (moved) {
            check(near(moved->at(0).positionMs, 0.0),
                  "the main cue goes to the start and never before it, got " + std::to_string(moved->at(0).positionMs));
        }
        // Not near the start: by the grid's move at its beat.
        const CueGridPlan later = plan({memory(1003.0)});
        const auto follows =
            ReanalyseTrackChange::engineCuesOntoNewGrid(later, {memory(1001.5)}, oldBeats, newBeats, tolerance);
        check(follows && near(follows->at(0).positionMs, 999.3), "the main cue follows by the grid's move");
        std::cout << "case 4 (Engine's main cue follows by the grid's move, never before the start) done\n";
    }

    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "reanalyse_cue_comparison_test: all passed\n";
    return 0;
}
