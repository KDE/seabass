// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/clustered_cue.hpp"

#include <algorithm>

#include "domain/junk_cue.hpp"

namespace seabass::domain
{

std::vector<ClusteredCueIssue> ClusteredCueFinder::find(const std::vector<Track> &tracks)
{
    std::vector<ClusteredCueIssue> issues;
    for (const Track &track : tracks) {
        std::vector<CuePoint> early;
        for (const CuePoint &cue : track.cues) {
            if (cue.kind != CuePoint::Kind::Hot || cue.isLoop) {
                continue;
            }
            // A negative position is a "no cue set" sentinel read as a
            // position (junk_cue.hpp). It points nowhere, so it is not
            // evidence that somebody's pads were written over.
            if (cue.positionMs < 0.0 || cue.positionMs >= ClusterWindowMs) {
                continue;
            }
            early.push_back(cue);
        }
        if (early.size() < ClusterMinimumCues) {
            continue;
        }
        std::sort(early.begin(), early.end(),
                  [](const CuePoint &a, const CuePoint &b) { return a.positionMs < b.positionMs; });
        issues.push_back({track, std::move(early)});
    }
    return issues;
}

std::vector<CuePoint> removableClusterCues(const ClusteredCueIssue &issue)
{
    std::vector<CuePoint> removable;
    for (const CuePoint &cue : issue.cluster) {
        if (isJunkCue(cue)) {
            continue;  // already offered by the stray-cue check
        }
        removable.push_back(cue);
    }
    return removable;
}

}  // namespace seabass::domain
