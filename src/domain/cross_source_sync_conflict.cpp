// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/cross_source_sync_conflict.hpp"

#include <map>
#include <utility>

#include "domain/track_matching.hpp"

namespace seabass::domain
{

namespace
{

struct PlanSides
{
    const Track *target;
    const Track *source;
};

PlanSides sidesOf(const SyncPlan &plan)
{
    bool toB = plan.direction == SyncPlan::Direction::ToB;
    return {toB ? &plan.match.trackB : &plan.match.trackA, toB ? &plan.match.trackA : &plan.match.trackB};
}

bool hasJunkCue(const std::vector<CuePoint> &cues)
{
    for (const auto &cue : cues) {
        if (cue.kind == CuePoint::Kind::Memory && cue.positionMs == 0.0) {
            return true;
        }
    }
    return false;
}

}  // namespace

CrossSourceConflictSplit CrossSourceConflictDetector::detect(const std::vector<SyncPlan> &actionablePlans)
{
    CrossSourceConflictSplit result;

    std::map<std::pair<std::string, std::string>, std::vector<const SyncPlan *>> byTarget;
    for (const auto &plan : actionablePlans) {
        const Track *target = sidesOf(plan).target;
        byTarget[{target->format, target->filePath}].push_back(&plan);
    }

    for (const auto &[key, plans] : byTarget) {
        if (plans.size() != 2) {
            // Either untouched by any other pair (the common case) or,
            // in principle, more sources than this codebase's current
            // three-catalog ceiling allows for one target -- see this
            // detector's own class comment. Neither case is a conflict
            // this detector can resolve, so every one of these plans
            // passes through unchanged.
            for (const auto *plan : plans) {
                result.nonConflicting.push_back(*plan);
            }
            continue;
        }

        const SyncPlan &planA = *plans[0];
        const SyncPlan &planB = *plans[1];
        if (cueSetsEqual(planA.cuesToApply, planB.cuesToApply)) {
            result.nonConflicting.push_back(planA);
            continue;
        }

        PlanSides a = sidesOf(planA);
        PlanSides b = sidesOf(planB);
        CrossSourceSyncConflict conflict;
        conflict.target = *a.target;
        conflict.sourceA = *a.source;
        conflict.cuesFromA = planA.cuesToApply;
        conflict.sourceAHasJunkCue = hasJunkCue(conflict.cuesFromA);
        conflict.sourceB = *b.source;
        conflict.cuesFromB = planB.cuesToApply;
        conflict.sourceBHasJunkCue = hasJunkCue(conflict.cuesFromB);
        result.conflicts.push_back(std::move(conflict));
    }

    return result;
}

}  // namespace seabass::domain
