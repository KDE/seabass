// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/sync_planning.hpp"

#include "domain/track_matching.hpp"

namespace seabass::domain
{

std::vector<SyncMatch> TrackMatcher::match(const std::vector<Track> &tracksA, const std::vector<Track> &tracksB)
{
    std::vector<SyncMatch> matches;
    for (const auto &[trackA, trackB] : matchTracks(tracksA, tracksB)) {
        matches.push_back({*trackA, *trackB});
    }
    return matches;
}

SyncPlan SyncPlanner::plan(const SyncMatch &match, std::chrono::system_clock::time_point mtimeA,
                            std::chrono::system_clock::time_point mtimeB)
{
    SyncPlan result;
    result.match = match;

    bool aHasCues = !match.trackA.cues.empty();
    bool bHasCues = !match.trackB.cues.empty();

    if (!aHasCues && !bHasCues) {
        result.kind = SyncPlan::Kind::NoCues;
        return result;
    }

    if (aHasCues && !bHasCues) {
        result.kind = SyncPlan::Kind::AOnly;
        result.direction = SyncPlan::Direction::ToB;
        result.cuesToApply = match.trackA.cues;
        return result;
    }

    if (!aHasCues && bHasCues) {
        result.kind = SyncPlan::Kind::BOnly;
        result.direction = SyncPlan::Direction::ToA;
        result.cuesToApply = match.trackB.cues;
        return result;
    }

    // Both sides have cues.
    if (cueSetsEqual(match.trackA.cues, match.trackB.cues)) {
        result.kind = SyncPlan::Kind::AlreadyConsistent;
        return result;
    }

    result.kind = SyncPlan::Kind::Conflict;
    if (mtimeA > mtimeB) {
        result.direction = SyncPlan::Direction::ToB;
        result.cuesToApply = match.trackA.cues;
    } else {
        result.direction = SyncPlan::Direction::ToA;
        result.cuesToApply = match.trackB.cues;
    }
    return result;
}

}  // namespace seabass::domain
