// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Cues and loops on a beat of a track's old grid, carried to the beat of
// the same number in its new one (Analyse again):
//
// 1. A cue on a beat moves with it; one off every beat stays; one within
//    5 ms of a beat moves and keeps its offset from it.
// 2. A loop keeps its length in beats: an end on a beat follows its own
//    beat, an end off the beats moves by the start's amount.
// 3. A move under 1 ms is no move.
// 4. A grid at another tempo, one shifted by half a beat, one shorter
//    than the cue's beat: GridChangedTooMuch, nothing moves.
// 5. No old grid: nothing moves.
// 6. cuesMovedOntoNewGrid() moves exactly the planned cues, colour, name
//    and pad kept.
// 7. The track whose cues follow on their own: its largest move, a loop's
//    end counted, earlier moves by their amount, under the threshold (at
//    it is not under); a threshold of 0 asks always; a plan that moves
//    nothing, or whose grid changed too much, never follows.
// 8. A move counts as stored too: rekordbox keeps whole ms, each position
//    cut, so 1100.0 to 1090.7 is a move of 10, not of 9.3.
// 9. A start that would come out before 0 lands on 0; a loop held there
//    ends on its beat when its end was on one, and keeps its length when
//    not; the amounts counted are the moves as made; a cue whose held
//    move is under 1 ms stays, kept in the plan as the move it would make.
// 10. The reviewer's two tracks near the start: an intro loop at 2 ms in
//    rekordbox and 2.4 ms in Engine, a cue at 1.1 ms and its copy at
//    0.8 ms, the grid moving by -2.2 ms: the same beats either way.

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

#include "application/use_cases/carry_cues_to_new_grid.hpp"

using namespace seabass;
using application::CueGridPlan;

namespace
{

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
    cue.color = "#FF0000";
    cue.comment = "pad " + std::to_string(pad);
    return cue;
}

domain::CuePoint memory(double ms)
{
    domain::CuePoint cue;
    cue.positionMs = ms;
    return cue;
}

domain::CuePoint loop(double from, double to)
{
    domain::CuePoint cue = hot(4, from);
    cue.isLoop = true;
    cue.loopEndMs = to;
    return cue;
}

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-6;
}

}  // namespace

int main()
{
    // 500 ms a beat from 100 ms; the new grid the same, 40 ms later.
    const std::vector<double> oldGrid = grid(100.0, 500.0, 40);
    const std::vector<double> later = grid(140.0, 500.0, 40);

    // 1.
    {
        const std::vector<domain::CuePoint> cues{hot(1, 1100.0), memory(1350.0), hot(2, 2103.0), memory(2096.0)};
        const CueGridPlan plan = application::planCueGridMoves(cues, oldGrid, later);
        assert(plan.verdict == CueGridPlan::Verdict::Moves);
        assert(plan.moves.size() == 3 && plan.offBeat == 1 && plan.alreadyThere == 0);
        assert(plan.moves[0].cueIndex == 0 && plan.moves[0].beat == 2 && near(plan.moves[0].newMs, 1140.0));
        assert(plan.moves[1].cueIndex == 2 && plan.moves[1].beat == 4 && near(plan.moves[1].newMs, 2143.0));
        assert(plan.moves[2].cueIndex == 3 && near(plan.moves[2].newMs, 2136.0));
        // 5 ms is on it, 5.1 is not.
        assert(application::planCueGridMoves({memory(1105.0)}, oldGrid, later).moves.size() == 1);
        assert(application::planCueGridMoves({memory(1105.1)}, oldGrid, later).offBeat == 1);
        std::cout << "case 1 (on a beat moves with it, off every beat stays, within 5 ms keeps its offset) OK\n";
    }

    // 2.
    {
        // On beats 2 and 6: both ends follow their beats.
        const std::vector<double> slower = [] {
            std::vector<double> beats = grid(140.0, 500.0, 40);
            for (std::size_t i = 0; i < beats.size(); ++i) {
                beats[i] += static_cast<double>(i) * 2.0;  // 2 ms longer a beat
            }
            return beats;
        }();
        CueGridPlan plan = application::planCueGridMoves({loop(1100.0, 3100.0)}, oldGrid, slower);
        assert(plan.moves.size() == 1 && plan.moves[0].isLoop && plan.moves[0].endBeat == 6u);
        assert(near(plan.moves[0].newMs, 1144.0) && near(plan.moves[0].newEndMs, 3152.0));
        // An end off the beats moves by the start's amount.
        plan = application::planCueGridMoves({loop(1100.0, 1350.0)}, oldGrid, slower);
        assert(plan.moves.size() == 1 && !plan.moves[0].endBeat && near(plan.moves[0].newEndMs, 1394.0));
        // A loop whose start is off the beats stays, however its end lies.
        plan = application::planCueGridMoves({loop(1350.0, 2100.0)}, oldGrid, slower);
        assert(plan.moves.empty() && plan.offBeat == 1);
        std::cout << "case 2 (a loop keeps its length in beats) OK\n";
    }

    // 3.
    {
        const CueGridPlan plan = application::planCueGridMoves({hot(1, 1100.0)}, oldGrid, grid(100.6, 500.0, 40));
        assert(plan.moves.empty() && plan.alreadyThere == 1 && plan.verdict == CueGridPlan::Verdict::NothingMoves);
        assert(application::planCueGridMoves({hot(1, 1100.0)}, oldGrid, grid(101.2, 500.0, 40)).moves.size() == 1);
        std::cout << "case 3 (under 1 ms is no move) OK\n";
    }

    // 4.
    {
        const std::vector<domain::CuePoint> cues{hot(1, 1100.0), hot(2, 12100.0)};
        // At 3% another tempo, beat 24 is 360 ms off: more than 125.
        CueGridPlan plan = application::planCueGridMoves(cues, oldGrid, grid(100.0, 485.0, 40));
        assert(plan.verdict == CueGridPlan::Verdict::GridChangedTooMuch && plan.moves.empty());
        plan = application::planCueGridMoves(cues, oldGrid, grid(350.0, 500.0, 40));
        assert(plan.verdict == CueGridPlan::Verdict::GridChangedTooMuch && plan.moves.empty());
        plan = application::planCueGridMoves(cues, oldGrid, grid(140.0, 500.0, 10));
        assert(plan.verdict == CueGridPlan::Verdict::GridChangedTooMuch && plan.moves.empty());
        // A quarter of a beat is still the same beat.
        plan = application::planCueGridMoves(cues, oldGrid, grid(224.0, 500.0, 40));
        assert(plan.verdict == CueGridPlan::Verdict::Moves && plan.moves.size() == 2);
        // A loop whose end beat cannot be trusted: too much as well.
        plan = application::planCueGridMoves({loop(1100.0, 18100.0)}, oldGrid, grid(100.0, 496.0, 40));
        assert(plan.verdict == CueGridPlan::Verdict::GridChangedTooMuch);
        std::cout << "case 4 (another tempo, half a beat off, a shorter grid: nothing moves) OK\n";
    }

    // 5.
    {
        const CueGridPlan plan = application::planCueGridMoves({hot(1, 1100.0)}, {}, later);
        assert(plan.verdict == CueGridPlan::Verdict::NoOldGrid && plan.moves.empty());
        assert(application::planCueGridMoves({hot(1, 100.0)}, {100.0}, later).verdict == CueGridPlan::Verdict::NoOldGrid);
        std::cout << "case 5 (no old grid: nothing moves) OK\n";
    }

    // 6.
    {
        const std::vector<domain::CuePoint> cues{hot(1, 1100.0), memory(1350.0), loop(2100.0, 3100.0)};
        const CueGridPlan plan = application::planCueGridMoves(cues, oldGrid, later);
        const std::vector<domain::CuePoint> moved = application::cuesMovedOntoNewGrid(cues, plan);
        assert(moved.size() == 3);
        assert(near(moved[0].positionMs, 1140.0) && moved[0].color == "#FF0000" && moved[0].comment == "pad 1"
               && moved[0].hotCueNumber == 1 && moved[0].kind == domain::CuePoint::Kind::Hot);
        assert(near(moved[1].positionMs, 1350.0));
        assert(near(moved[2].positionMs, 2140.0) && near(moved[2].loopEndMs, 3140.0) && moved[2].hotCueNumber == 4);
        std::cout << "case 6 (the planned cues moved, the rest of each cue kept) OK\n";
    }

    // 7.
    {
        // Each beat 1 ms further than the one before, beat 0 by 4 ms: a
        // loop from beat 2 (6 ms) to beat 8 (12 ms).
        const std::vector<double> stretched = [] {
            std::vector<double> beats = grid(104.0, 500.0, 40);
            for (std::size_t i = 0; i < beats.size(); ++i) {
                beats[i] += static_cast<double>(i);
            }
            return beats;
        }();
        const CueGridPlan withLoop =
            application::planCueGridMoves({hot(1, 1100.0), loop(1100.0, 4100.0)}, oldGrid, stretched);
        assert(withLoop.verdict == CueGridPlan::Verdict::Moves && withLoop.moves.size() == 2);
        assert(near(application::largestCueMoveMs(withLoop), 12.0));
        // The start alone (6 ms) is under 10; the loop's end is not.
        assert(!application::cuesFollowOnTheirOwn(withLoop, 10.0));
        assert(!application::cuesFollowOnTheirOwn(withLoop, 12.0));
        assert(application::cuesFollowOnTheirOwn(withLoop, 12.5));
        // Earlier by 8 ms is a move of 8.
        const CueGridPlan earlier = application::planCueGridMoves({hot(1, 1100.0)}, oldGrid, grid(92.0, 500.0, 40));
        assert(near(application::largestCueMoveMs(earlier), 8.0));
        assert(application::cuesFollowOnTheirOwn(earlier, 10.0));
        assert(!application::cuesFollowOnTheirOwn(earlier, 8.0));
        assert(!application::cuesFollowOnTheirOwn(earlier, 0.0));
        // Nothing to move, or a grid not to be trusted: never.
        const CueGridPlan nothing = application::planCueGridMoves({memory(1350.0)}, oldGrid, later);
        assert(nothing.verdict == CueGridPlan::Verdict::NothingMoves && application::largestCueMoveMs(nothing) == 0.0);
        assert(!application::cuesFollowOnTheirOwn(nothing, 100.0));
        const CueGridPlan tooMuch = application::planCueGridMoves({hot(1, 1100.0)}, oldGrid, grid(350.0, 500.0, 40));
        assert(!application::cuesFollowOnTheirOwn(tooMuch, 100.0));
        std::cout << "case 7 (the largest move, a loop's end too, under the threshold follows on its own; at it, or "
                     "at 0, asks) OK\n";
    }

    // 8.
    {
        // The new grid 9.3 ms earlier. A cue at 1100.0 lands at 1090.7,
        // stored as 1090: a move of 10 whole ms, not under 10.
        const std::vector<double> earlier = grid(90.7, 500.0, 40);
        const CueGridPlan whole = application::planCueGridMoves({hot(1, 1100.0)}, oldGrid, earlier);
        assert(whole.verdict == CueGridPlan::Verdict::Moves && whole.moves.size() == 1);
        assert(near(whole.moves[0].newMs - whole.moves[0].oldMs, -9.3));
        assert(near(application::cueMoveMs(1100.0, 1090.7), 10.0));
        assert(near(application::largestCueMoveMs(whole), 10.0));
        assert(!application::cuesFollowOnTheirOwn(whole, 10.0));
        assert(application::cuesFollowOnTheirOwn(whole, 11.0));
        // From 1100.5 to 1091.2 is stored as 1100 to 1091: 9 whole ms, and
        // the move itself, 9.3, is the larger.
        const CueGridPlan under = application::planCueGridMoves({hot(1, 1100.5)}, oldGrid, earlier);
        assert(near(application::largestCueMoveMs(under), 9.3));
        assert(application::cuesFollowOnTheirOwn(under, 10.0));
        // A loop whose start is stored 9 ms earlier and whose end 10: the
        // end decides.
        const CueGridPlan withLoop = application::planCueGridMoves({loop(2100.5, 3100.0)}, oldGrid, earlier);
        assert(withLoop.moves.size() == 1 && withLoop.moves[0].isLoop);
        assert(near(application::largestCueMoveMs(withLoop), 10.0));
        assert(!application::cuesFollowOnTheirOwn(withLoop, 10.0));
        std::cout << "case 8 (a move just under the threshold that is stored as a whole ms at it does not follow on "
                     "its own; a loop's stored end counts) OK\n";
    }

    // 9.
    {
        // The old grid's first beat at 4 ms, the new one's at 0: a cue on
        // that beat at 1 ms would land at -3 ms, and lands on 0 instead.
        const std::vector<double> late = grid(4.0, 500.0, 40);
        const std::vector<double> early = grid(0.0, 500.0, 40);
        const CueGridPlan plan =
            application::planCueGridMoves({memory(1.0), loop(2.0, 504.0), hot(2, 0.5), hot(3, 6.0)}, late, early);
        assert(plan.verdict == CueGridPlan::Verdict::Moves);
        // 0.5 ms would move by -0.5 ms once held at 0: no move.
        assert(plan.alreadyThere == 1);
        assert(plan.moves.size() == 3);
        for (const auto &move : plan.moves) {
            assert(move.newMs >= 0.0 && (!move.isLoop || move.newEndMs >= 0.0));
        }
        assert(plan.moves[0].cueIndex == 0 && near(plan.moves[0].newMs, 0.0));
        // The loop's start held at 0 moved by 2 ms; its end, on beat 1,
        // still follows its beat: 504 to 500, the loop's beat of the new
        // grid, not 502 as the length in ms would have it.
        const auto &held = plan.moves[1];
        assert(held.isLoop && near(held.newMs, 0.0) && near(held.newEndMs, 500.0));
        assert(held.endBeat == 1u);
        // Not held: 6.0 moves by the full -4 ms.
        assert(plan.moves[2].cueIndex == 3 && near(plan.moves[2].newMs, 2.0));
        // The amounts are the moves as made: the held cue moved 1 ms, the
        // held loop's start 2 ms and its end 4 ms, where the grid moved 4.
        const CueGridPlan heldOnly = application::planCueGridMoves({memory(1.0), loop(2.0, 504.0)}, late, early);
        assert(near(application::largestCueMoveMs(heldOnly), 4.0));
        assert(application::cuesFollowOnTheirOwn(heldOnly, 5.0));
        // A held loop whose end is on no beat keeps its length in ms.
        const CueGridPlan offBeatEnd = application::planCueGridMoves({loop(2.0, 254.0)}, late, early);
        assert(offBeatEnd.moves.size() == 1 && !offBeatEnd.moves[0].endBeat);
        assert(near(offBeatEnd.moves[0].newMs, 0.0) && near(offBeatEnd.moves[0].newEndMs, 252.0));
        // The cue under a millisecond once held is kept as the move it
        // would have made, for a comparison of two plans.
        assert(plan.stayed.size() == 1 && plan.stayed[0].cueIndex == 2 && plan.stayed[0].beat == 0
               && near(plan.stayed[0].newMs, 0.0));
        const std::vector<domain::CuePoint> moved = application::cuesMovedOntoNewGrid(
            {memory(1.0), loop(2.0, 504.0), hot(2, 0.5), hot(3, 6.0)}, plan);
        for (const auto &cue : moved) {
            assert(cue.positionMs >= 0.0 && (!cue.isLoop || cue.loopEndMs >= 0.0));
        }
        assert(near(moved[2].positionMs, 0.5) && "a cue that stayed is not moved");
        std::cout << "case 9 (a start that would come out before 0 lands on 0; a loop held there ends on its beat, "
                     "or keeps its length when its end is on none) OK\n";
    }

    // 10. One track in two libraries near its start, the grid moving by
    //     -2.2 ms: rekordbox keeps whole ms, Engine samples, so the hold
    //     at 0 meets each copy differently. Neither changes which beat a
    //     cue or a loop's end belongs to.
    {
        const std::vector<double> oldBeats = grid(2.2, 500.0, 40);
        const std::vector<double> newBeats = grid(0.0, 500.0, 40);
        // An intro loop over four beats at 2 ms in rekordbox, 2.4 ms in
        // Engine: rekordbox's start is held at 0, Engine's is not. Both
        // end on beat 4 of the new grid, the offset from it kept.
        const CueGridPlan rekordbox = application::planCueGridMoves({loop(2.0, 2002.0)}, oldBeats, newBeats);
        const CueGridPlan engine = application::planCueGridMoves({loop(2.4, 2002.4)}, oldBeats, newBeats);
        assert(rekordbox.moves.size() == 1 && engine.moves.size() == 1);
        assert(near(rekordbox.moves[0].newMs, 0.0) && near(engine.moves[0].newMs, 0.2));
        assert(rekordbox.moves[0].endBeat == 4u && engine.moves[0].endBeat == 4u);
        assert(near(rekordbox.moves[0].newEndMs, 1999.8) && near(engine.moves[0].newEndMs, 2000.2));
        // A cue at 1.1 ms and its copy at 0.8 ms: both land on 0, one a
        // move of 1.1 ms, the other under the minimum. The second stays,
        // on the same beat going to the same place.
        const CueGridPlan moves = application::planCueGridMoves({hot(2, 1.1)}, oldBeats, newBeats);
        const CueGridPlan stays = application::planCueGridMoves({hot(2, 0.8)}, oldBeats, newBeats);
        assert(moves.moves.size() == 1 && moves.stayed.empty() && near(moves.moves[0].newMs, 0.0));
        assert(stays.moves.empty() && stays.alreadyThere == 1 && stays.stayed.size() == 1);
        assert(stays.stayed[0].beat == moves.moves[0].beat && near(stays.stayed[0].newMs, moves.moves[0].newMs));
        std::cout << "case 10 (near the start, the hold at 0 changes amounts only: a loop's end keeps its beat, "
                     "a cue under a millisecond is kept as the move it would make) OK\n";
    }

    std::cout << "carry_cues_to_new_grid_test: all cases passed\n";
    return 0;
}
