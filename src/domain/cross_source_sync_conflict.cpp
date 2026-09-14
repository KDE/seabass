// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/cross_source_sync_conflict.hpp"

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

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
        // A target with no file (or a streaming one) shares its empty path
        // with every other such row. Grouped, two unrelated plans would read
        // as one conflict and resolving it would write one track's cues onto
        // the other. It can only be one plan's target, so it passes through.
        if (target->filePath.empty() || !target->streamingSource.empty()) {
            result.nonConflicting.push_back(plan);
            continue;
        }
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

std::vector<CrossSourceSyncConflict> CrossSourceConflictDetector::takeHotCueChoices(std::vector<SyncPlan> &plans,
                                                                                    const FileKey &fileKey)
{
    const auto involves = [](const SyncPlan &plan, const std::string &format) {
        return plan.match.trackA.format == format || plan.match.trackB.format == format;
    };
    // Both sides, not just trackA: see the header for why one side's path
    // is not "the file". A streaming track has no file of its own, and its
    // path can come out as the Engine Library folder itself, which every
    // streaming track would share -- keyed, two unrelated tracks would merge
    // into one card and hold each other's plans back.
    const auto keysOf = [&fileKey](const SyncPlan &plan) {
        std::vector<std::string> keys;
        for (const Track *track : {&plan.match.trackA, &plan.match.trackB}) {
            if (track->filePath.empty() || !track->streamingSource.empty()) {
                continue;
            }
            std::string key = fileKey(track->filePath);
            if (!key.empty()) {
                keys.push_back(std::move(key));
            }
        }
        return keys;
    };

    // Keys joined by a choice form one group: a choice between rekordbox's
    // path and a title-matched Engine path makes both of them the same track.
    std::map<std::string, std::string> parent;
    const auto root = [&parent](std::string key) {
        while (parent.at(key) != key) {
            key = parent.at(key);
        }
        return key;
    };

    std::vector<std::pair<const SyncPlan *, std::string>> choicePlans;
    std::vector<const SyncPlan *> choicesWithoutFile;
    for (const auto &plan : plans) {
        if (!plan.hotCuesNeedChoice) {
            continue;
        }
        const std::vector<std::string> keys = keysOf(plan);
        if (keys.empty()) {
            choicesWithoutFile.push_back(&plan);
            continue;
        }
        for (const std::string &key : keys) {
            parent.emplace(key, key);
            const std::string joined = root(keys.front());
            const std::string other = root(key);
            if (joined != other) {
                parent[other] = joined;
            }
        }
        choicePlans.emplace_back(&plan, keys.front());
    }

    // One choice per group: prefer the pair with rekordbox in it.
    std::map<std::string, const SyncPlan *> choiceByGroup;
    for (const auto &[plan, key] : choicePlans) {
        auto [it, inserted] = choiceByGroup.emplace(root(key), plan);
        if (!inserted && involves(*plan, "rekordbox") && !involves(*it->second, "rekordbox")) {
            it->second = plan;
        }
    }

    const auto toConflict = [](const SyncPlan &plan) {
        CrossSourceSyncConflict choice;
        choice.samePair = true;
        choice.target = plan.match.trackA;
        choice.sourceA = plan.match.trackA;
        choice.cuesFromA = plan.cuesIfAWins;
        choice.sourceAHasJunkCue = hasJunkCue(plan.match.trackA.cues);
        choice.sourceB = plan.match.trackB;
        choice.cuesFromB = plan.cuesIfBWins;
        choice.sourceBHasJunkCue = hasJunkCue(plan.match.trackB.cues);
        return choice;
    };

    std::vector<CrossSourceSyncConflict> choices;
    for (const auto &[group, plan] : choiceByGroup) {
        choices.push_back(toConflict(*plan));
    }
    for (const auto *plan : choicesWithoutFile) {
        choices.push_back(toConflict(*plan));
    }

    // Everything else with either side on a file with an open choice waits
    // for it.
    std::vector<SyncPlan> rest;
    rest.reserve(plans.size());
    for (auto &plan : plans) {
        if (plan.hotCuesNeedChoice) {
            continue;
        }
        const std::vector<std::string> keys = keysOf(plan);
        const bool waits = std::any_of(keys.begin(), keys.end(),
                                       [&parent](const std::string &key) { return parent.count(key) != 0; });
        if (waits) {
            continue;
        }
        rest.push_back(std::move(plan));
    }
    plans = std::move(rest);
    return choices;
}

}  // namespace seabass::domain
