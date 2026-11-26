// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/carry_cues_to_new_grid.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace seabass::application
{

namespace
{

// The beat of `beats` within CueOnBeatToleranceMs of `ms`, the nearest.
std::optional<std::size_t> beatAt(const std::vector<double> &beats, double ms)
{
    const auto after = std::lower_bound(beats.begin(), beats.end(), ms);
    std::optional<std::size_t> best;
    double distance = CueOnBeatToleranceMs;
    for (auto at = after == beats.begin() ? after : after - 1; at != beats.end() && at <= after; ++at) {
        const double d = std::abs(*at - ms);
        if (d <= distance) {
            distance = d;
            best = static_cast<std::size_t>(at - beats.begin());
        }
    }
    return best;
}

double spacingAt(const std::vector<double> &beats, std::size_t i)
{
    return i + 1 < beats.size() ? beats[i + 1] - beats[i] : beats[i] - beats[i - 1];
}

// The same beat in the new grid, trusted: there, and within a quarter of
// a beat of the old one.
bool trusted(const std::vector<double> &oldBeats, const std::vector<double> &newBeats, std::size_t beat)
{
    return beat < newBeats.size() && std::abs(newBeats[beat] - oldBeats[beat]) <= spacingAt(oldBeats, beat) / 4.0;
}

}  // namespace

CueGridPlan planCueGridMoves(const std::vector<domain::CuePoint> &cues, const std::vector<double> &oldBeats,
                             const std::vector<double> &newBeats)
{
    CueGridPlan plan;
    if (oldBeats.size() < 2) {
        return plan;
    }
    bool untrusted = false;
    for (std::size_t i = 0; i < cues.size(); ++i) {
        const domain::CuePoint &cue = cues[i];
        const auto beat = beatAt(oldBeats, cue.positionMs);
        if (!beat) {
            ++plan.offBeat;
            continue;
        }
        if (!trusted(oldBeats, newBeats, *beat)) {
            untrusted = true;
            continue;
        }
        CueGridMove move;
        move.cueIndex = i;
        move.kind = cue.kind;
        move.hotCueNumber = cue.hotCueNumber;
        move.isLoop = cue.isLoop;
        move.color = cue.color;
        move.comment = cue.comment;
        move.beat = *beat;
        move.oldMs = cue.positionMs;
        const double shift = newBeats[*beat] - oldBeats[*beat];
        move.newMs = cue.positionMs + shift;
        double endShift = shift;
        if (cue.isLoop) {
            move.oldEndMs = cue.loopEndMs;
            if (const auto end = beatAt(oldBeats, cue.loopEndMs)) {
                if (!trusted(oldBeats, newBeats, *end)) {
                    untrusted = true;
                    continue;
                }
                move.endBeat = end;
                endShift = newBeats[*end] - oldBeats[*end];
            }
            move.newEndMs = cue.loopEndMs + endShift;
        }
        if (std::abs(shift) < CueMoveMinimumMs && std::abs(endShift) < CueMoveMinimumMs) {
            ++plan.alreadyThere;
            continue;
        }
        plan.moves.push_back(move);
    }
    if (untrusted) {
        plan.verdict = CueGridPlan::Verdict::GridChangedTooMuch;
        plan.moves.clear();
    } else {
        plan.verdict = plan.moves.empty() ? CueGridPlan::Verdict::NothingMoves : CueGridPlan::Verdict::Moves;
    }
    return plan;
}

double largestCueMoveMs(const CueGridPlan &plan)
{
    double largest = 0.0;
    for (const CueGridMove &move : plan.moves) {
        largest = std::max(largest, std::abs(move.newMs - move.oldMs));
        if (move.isLoop) {
            largest = std::max(largest, std::abs(move.newEndMs - move.oldEndMs));
        }
    }
    return largest;
}

bool cuesFollowOnTheirOwn(const CueGridPlan &plan, double thresholdMs)
{
    return plan.verdict == CueGridPlan::Verdict::Moves && !plan.moves.empty() && largestCueMoveMs(plan) < thresholdMs;
}

std::vector<domain::CuePoint> cuesMovedOntoNewGrid(std::vector<domain::CuePoint> cues, const CueGridPlan &plan)
{
    for (const CueGridMove &move : plan.moves) {
        domain::CuePoint &cue = cues.at(move.cueIndex);
        cue.positionMs = move.newMs;
        if (cue.isLoop) {
            cue.loopEndMs = move.newEndMs;
        }
    }
    return cues;
}

}  // namespace seabass::application
