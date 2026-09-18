// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/junk_cue.hpp"

namespace seabass::domain
{

bool isJunkMemoryCue(const CuePoint &cue)
{
    return cue.kind == CuePoint::Kind::Memory && cue.positionMs < 1000.0;
}

std::vector<JunkCueIssue> JunkCueFinder::find(const std::vector<Track> &tracks)
{
    std::vector<JunkCueIssue> issues;
    for (const auto &track : tracks) {
        for (const auto &cue : track.cues) {
            if (isJunkMemoryCue(cue)) {
                issues.push_back(JunkCueIssue{track, cue});
            }
        }
    }
    return issues;
}

std::vector<CuePoint> withoutJunkMemoryCues(const std::vector<CuePoint> &cues)
{
    std::vector<CuePoint> kept;
    kept.reserve(cues.size());
    for (const CuePoint &cue : cues) {
        if (!isJunkMemoryCue(cue)) {
            kept.push_back(cue);
        }
    }
    return kept;
}

}  // namespace seabass::domain
