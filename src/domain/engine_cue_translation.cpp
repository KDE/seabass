// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/engine_cue_translation.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace seabass::domain
{

namespace
{

// The tolerance cueSetsEqual() allows (track_matching.cpp), so a cue
// that drifted by a cross-format rounding is still the same cue.
constexpr double PositionToleranceMs = 500.0;

bool samePlace(const CuePoint &a, const CuePoint &b)
{
    return a.isLoop == b.isLoop && std::abs(a.positionMs - b.positionMs) <= PositionToleranceMs;
}

bool isHot(const CuePoint &cue)
{
    return cue.kind == CuePoint::Kind::Hot;
}

bool padIsValid(int pad)
{
    return pad >= 1 && pad <= EngineHotCuePads;
}

}  // namespace

EngineCueTranslation translateCuesForEngine(const std::vector<CuePoint> &cues, const std::vector<CuePoint> &existing)
{
    EngineCueTranslation result;
    bool padTaken[EngineHotCuePads + 1] = {};

    // Hot cues and hot loops keep their pads.
    for (const CuePoint &cue : cues) {
        if (isHot(cue) && padIsValid(cue.hotCueNumber)) {
            padTaken[cue.hotCueNumber] = true;
            result.cues.push_back(cue);
        }
    }

    // Memory cues first, then memory loops, each in time order.
    std::vector<CuePoint> memory;
    for (const CuePoint &cue : cues) {
        if (!isHot(cue)) {
            memory.push_back(cue);
        }
    }
    std::stable_sort(memory.begin(), memory.end(), [](const CuePoint &a, const CuePoint &b) {
        if (a.isLoop != b.isLoop) {
            return !a.isLoop;
        }
        return a.positionMs < b.positionMs;
    });

    std::optional<CuePoint> existingMain;
    for (const CuePoint &cue : existing) {
        if (!isHot(cue)) {
            existingMain = cue;
            break;
        }
    }

    for (const CuePoint &cue : memory) {
        // Already on a pad: one of the hot cues sits here.
        const bool onAPad = std::any_of(result.cues.begin(), result.cues.end(), [&](const CuePoint &hot) {
            return isHot(hot) && samePlace(hot, cue);
        });
        if (onAPad) {
            continue;
        }
        int pad = 0;
        // The pad Engine already holds this cue on, when it is still free.
        for (const CuePoint &had : existing) {
            if (isHot(had) && padIsValid(had.hotCueNumber) && !padTaken[had.hotCueNumber] && samePlace(had, cue)) {
                pad = had.hotCueNumber;
                break;
            }
        }
        for (int candidate = 1; pad == 0 && candidate <= EngineHotCuePads; ++candidate) {
            if (!padTaken[candidate]) {
                pad = candidate;
            }
        }
        if (pad == 0) {
            result.leftOut.push_back(cue);
            continue;
        }
        padTaken[pad] = true;
        CuePoint translated = cue;
        translated.kind = CuePoint::Kind::Hot;
        translated.hotCueNumber = pad;
        if (translated.color == "#000000") {
            translated.color.clear();
        }
        result.cues.push_back(translated);
    }

    // The main cue: Engine's own if it has one, else the earliest memory
    // cue Engine can hold. A loop is a loop, not a cue point.
    if (existingMain) {
        result.cues.push_back(*existingMain);
        return result;
    }
    const CuePoint *earliest = nullptr;
    for (const CuePoint &cue : memory) {
        if (cue.isLoop || !engineCanHoldMemoryCue(cue)) {
            continue;
        }
        if (!earliest || cue.positionMs < earliest->positionMs) {
            earliest = &cue;
        }
    }
    if (earliest) {
        CuePoint main = *earliest;
        main.kind = CuePoint::Kind::Memory;
        main.hotCueNumber = 0;
        result.cues.push_back(main);
    }
    return result;
}

CuesFromEngine cuesFromEngine(const std::vector<CuePoint> &engineCues, const std::vector<CuePoint> &cues)
{
    CuesFromEngine result;
    for (const CuePoint &cue : engineCues) {
        if (!isHot(cue)) {
            result.memoryCues.push_back(cue);
            continue;
        }
        const bool isOwnHotCue = std::any_of(cues.begin(), cues.end(), [&](const CuePoint &own) {
            return isHot(own) && own.hotCueNumber == cue.hotCueNumber && samePlace(own, cue);
        });
        const auto translationOf = std::find_if(cues.begin(), cues.end(), [&](const CuePoint &memory) {
            return memory.kind == CuePoint::Kind::Memory && samePlace(memory, cue);
        });
        if (isOwnHotCue || translationOf == cues.end()) {
            result.hotCues.push_back(cue);
            continue;
        }
        // The memory cue it translates, as that catalog knows it, so a
        // write back carries that catalog's own colour and comment.
        result.memoryCues.push_back(*translationOf);
    }
    return result;
}

}  // namespace seabass::domain
