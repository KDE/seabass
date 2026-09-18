// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/junk_cue.hpp"

namespace seabass::domain
{

bool isJunkCue(const CuePoint &cue)
{
    // A loop is never noise, wherever it starts: an intro loop set on the
    // first bar is a real thing a DJ places, and it has an end as well as
    // a start, which a stray press does not.
    return !cue.isLoop && cue.positionMs < 1000.0;
}

std::vector<JunkCueIssue> JunkCueFinder::find(const std::vector<Track> &tracks)
{
    std::vector<JunkCueIssue> issues;
    for (const auto &track : tracks) {
        for (const auto &cue : track.cues) {
            if (isJunkCue(cue)) {
                issues.push_back(JunkCueIssue{track, cue});
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
