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

    std::cout << "carry_cues_to_new_grid_test: all cases passed\n";
    return 0;
}
