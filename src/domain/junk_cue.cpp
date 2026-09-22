// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/junk_cue.hpp"

#include "domain/matching_policy.hpp"

namespace seabass::domain
{

bool isJunkCue(const CuePoint &cue)
{
    // A loop is never noise, wherever it starts: an intro loop set on the
    // first bar is a real thing a DJ places, and it has an end as well as
    // a start, which a stray press does not.
    if (cue.isLoop) {
        return false;
    }
    // A position before the start is junk whatever the user prefers, and
    // the preference is not about it. "Ignore cues at 0:00" is a
    // judgement call about cues that could conceivably have been placed
    // -- a DJ who keeps a "track start" pad turns it off and keeps them.
    // A negative position is not a judgement call: it is a format's "no
    // cue set" sentinel read as a position, and there is nowhere in the
    // track for it to point. Leaving those in on the user's behalf would
    // hand them a cue that cannot be navigated to.
    if (cue.positionMs < 0.0) {
        return true;
    }
    if (!MatchingPolicy::ignoreCuesAtStart()) {
        return false;
    }
    return cue.positionMs < 1000.0;
}

std::vector<JunkCueIssue> JunkCueFinder::find(const std::vector<Track> &tracks)
{
    std::vector<JunkCueIssue> issues;
    for (const auto &track : tracks) {
        for (const auto &cue : track.cues) {
            if (isJunkCue(cue)) {
                issues.push_back(JunkCueIssue{track, cue, "at the very start of the track"});
            }
        }
    }
    return issues;
}

std::vector<CuePoint> withoutJunkCues(const std::vector<CuePoint> &cues)
{
    std::vector<CuePoint> kept;
    kept.reserve(cues.size());
    for (const CuePoint &cue : cues) {
        if (!isJunkCue(cue)) {
            kept.push_back(cue);
        }
    }
    return kept;
}

}  // namespace seabass::domain
