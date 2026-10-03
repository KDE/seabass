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

// Within the planner's tolerance (domain/cue_tolerance.hpp), so a cue
// that drifted by a cross-format rounding is still the same cue.
bool samePlace(const CuePoint &a, const CuePoint &b, double toleranceMs)
{
    return a.isLoop == b.isLoop && std::abs(a.positionMs - b.positionMs) < toleranceMs;
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

EngineCueTranslation translateCuesForEngine(const std::vector<CuePoint> &cues, const std::vector<CuePoint> &existing,
                                            double toleranceMs)
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
            return isHot(hot) && samePlace(hot, cue, toleranceMs);
        });
        if (onAPad) {
            continue;
        }
        int pad = 0;
        // The pad Engine already holds this cue on, when it is still free.
        for (const CuePoint &had : existing) {
            if (isHot(had) && padIsValid(had.hotCueNumber) && !padTaken[had.hotCueNumber] && samePlace(had, cue, toleranceMs)) {
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

CuesFromEngine cuesFromEngine(const std::vector<CuePoint> &engineCues, const std::vector<CuePoint> &cues,
                              double toleranceMs)
{
    CuesFromEngine result;
    // The pads Engine DJ's own import gives the memory cues: free pads in
    // time order, nothing Engine had before taken into account. Worked out
    // only once a pad is found at a memory cue.
    std::optional<std::vector<CuePoint>> imported;
    const auto importPadOf = [&](const CuePoint &memory) {
        if (!imported) {
            imported = translateCuesForEngine(cues, {}, toleranceMs).cues;
        }
        for (const CuePoint &pad : *imported) {
            if (isHot(pad) && pad.positionMs == memory.positionMs && pad.isLoop == memory.isLoop) {
                return pad.hotCueNumber;
            }
        }
        return 0;
    };
    for (const CuePoint &cue : engineCues) {
        if (!isHot(cue)) {
            result.memoryCues.push_back(cue);
            continue;
        }
        const bool isOwnHotCue = std::any_of(cues.begin(), cues.end(), [&](const CuePoint &own) {
            return isHot(own) && own.hotCueNumber == cue.hotCueNumber && samePlace(own, cue, toleranceMs);
        });
        if (isOwnHotCue) {
            result.hotCues.push_back(cue);
            continue;
        }
        // The memory cue this pad translates. Of several within the
        // tolerance (two markers half a beat apart), the one the import
        // put on this very pad, else the nearest.
        const CuePoint *translationOf = nullptr;
        bool onImportsPad = false;
        for (const CuePoint &memory : cues) {
            if (memory.kind != CuePoint::Kind::Memory || !samePlace(memory, cue, toleranceMs)) {
                continue;
            }
            if (importPadOf(memory) == cue.hotCueNumber) {
                translationOf = &memory;
                onImportsPad = true;
                break;
            }
            if (translationOf == nullptr
                || std::abs(memory.positionMs - cue.positionMs) < std::abs(translationOf->positionMs - cue.positionMs)) {
                translationOf = &memory;
            }
        }
        if (translationOf == nullptr) {
            result.hotCues.push_back(cue);
            continue;
        }
        // The memory cue it translates, as that catalog knows it, so a
        // write back carries that catalog's own colour and comment.
        result.memoryCues.push_back(*translationOf);
        if (!onImportsPad) {
            result.uncertain.push_back({cue, *translationOf});
        }
    }
    return result;
}

std::vector<CuePoint> cuesInTermsOf(const std::vector<CuePoint> &engineCues, const std::vector<CuePoint> &cues,
                                    double toleranceMs)
{
    CuesFromEngine seen = cuesFromEngine(engineCues, cues, toleranceMs);
    std::vector<CuePoint> out = std::move(seen.hotCues);
    for (const CuePoint &memory : seen.memoryCues) {
        const bool already = std::any_of(out.begin(), out.end(), [&](const CuePoint &have) {
            return have.kind == CuePoint::Kind::Memory && samePlace(have, memory, toleranceMs);
        });
        if (!already) {
            CuePoint cue = memory;
            cue.kind = CuePoint::Kind::Memory;
            cue.hotCueNumber = 0;
            out.push_back(cue);
        }
    }
    return out;
}

}  // namespace seabass::domain
