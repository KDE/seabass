// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/junk_cue.hpp"

#include "domain/engine_cue_translation.hpp"
#include "domain/matching_policy.hpp"

#include <algorithm>

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

std::vector<StartCueOverEngine> startCuesOverEngine(const std::vector<CuePoint> &rekordboxCues,
                                                    const std::vector<CuePoint> &engineHotCues)
{
    std::vector<StartCueOverEngine> over;
    if (!MatchingPolicy::ignoreCuesAtStart()) {
        return over;
    }
    const auto onPad = [](const CuePoint &cue, int pad) {
        return cue.kind == CuePoint::Kind::Hot && cue.hotCueNumber == pad;
    };
    for (int pad = 1; pad <= EngineHotCuePads; ++pad) {
        const CuePoint *start = nullptr;
        bool realCue = false;
        for (const CuePoint &cue : rekordboxCues) {
            if (!onPad(cue, pad)) {
                continue;
            }
            if (isJunkCue(cue)) {
                start = start ? start : &cue;
            } else {
                realCue = true;
            }
        }
        if (!start || realCue) {
            continue;
        }
        const auto engine = std::find_if(engineHotCues.begin(), engineHotCues.end(), [&](const CuePoint &cue) {
            return onPad(cue, pad) && !isJunkCue(cue);
        });
        if (engine != engineHotCues.end()) {
            over.push_back(StartCueOverEngine{pad, *start, *engine});
        }
    }
    return over;
}

std::vector<CuePoint> withEngineCuesOverStartCues(const std::vector<CuePoint> &rekordboxCues,
                                                  const std::vector<StartCueOverEngine> &over)
{
    std::vector<CuePoint> cues;
    cues.reserve(rekordboxCues.size() + over.size());
    for (const CuePoint &cue : rekordboxCues) {
        const bool replaced = std::any_of(over.begin(), over.end(), [&](const StartCueOverEngine &o) {
            return cue.kind == CuePoint::Kind::Hot && cue.hotCueNumber == o.pad && isJunkCue(cue);
        });
        if (!replaced) {
            cues.push_back(cue);
        }
    }
    for (const StartCueOverEngine &o : over) {
        cues.push_back(o.engineCue);
    }
    return cues;
}

std::vector<JunkCueIssue> JunkCueFinder::find(const std::vector<Track> &tracks)
{
    std::vector<JunkCueIssue> issues;
    for (const auto &track : tracks) {
        for (const auto &cue : track.cues) {
            // Engine has no memory cues: its one "memory" cue is the
            // main cue, the player's CUE point, which Engine's analysis
            // places a few hundred milliseconds in by design (#61:
            // WHALESHARK2 listed The End @660 ms). Removing it is not
            // tidying a stray press, so it is never offered here. A
            // negative position is still the format's "unset" sentinel.
            if (track.format == "engine" && cue.kind == CuePoint::Kind::Memory && cue.positionMs >= 0.0) {
                continue;
            }
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
