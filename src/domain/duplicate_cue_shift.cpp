// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/duplicate_cue_shift.hpp"

#include <cmath>

namespace seabass::domain
{

bool groupCarriesCues(const DuplicateGroup &group)
{
    for (const auto &track : group.tracks) {
        if (!track.cues.empty()) {
            return true;
        }
        for (const auto &row : track.catalogRows) {
            if (!row.cues.empty()) {
                return true;
            }
        }
    }
    return false;
}

size_t cueShiftPairsToMeasure(const std::vector<DuplicateGroup> &groups)
{
    size_t pairs = 0;
    for (const auto &group : groups) {
        if (group.tracks.size() > 1 && groupCarriesCues(group)) {
            pairs += group.tracks.size() - 1;
        }
    }
    return pairs;
}

void measureCueShifts(std::vector<DuplicateGroup> &groups, AudioAlignmentProbe *probe,
                      const CueTimelineOffset &offset, const std::function<void()> &measured)
{
    for (auto &group : groups) {
        group.cueShiftMs.clear();
        if (group.tracks.size() < 2 || !groupCarriesCues(group)) {
            continue;
        }
        const Track &first = group.tracks.front();
        group.cueShiftMs.assign(group.tracks.size(), std::nullopt);
        group.cueShiftMs[0] = 0.0;
        for (size_t i = 1; i < group.tracks.size(); ++i) {
            const Track &copy = group.tracks[i];
            std::optional<double> decoderShift;
            if (!first.filePath.empty() && !copy.filePath.empty()) {
                if (copy.filePath == first.filePath
                    || (probe != nullptr && probe->sameContent(first.filePath, copy.filePath))) {
                    // One file listed twice, or two files with the same
                    // bytes: lined up on every timeline, whatever either
                    // file's offset between them is.
                    if (measured) {
                        measured();
                    }
                    group.cueShiftMs[i] = 0.0;
                    continue;
                }
                if (probe != nullptr) {
                    decoderShift = probe->shiftMs(first.filePath, copy.filePath);
                }
            }
            if (measured) {
                measured();
            }
            if (!decoderShift) {
                continue;
            }
            // The decoder measured on its own timeline; cues are held on
            // rekordbox's. Where a sound is in b minus where it is in a,
            // on rekordbox's timeline, is the decoder's difference plus
            // the two files' own offsets between the two timelines.
            const std::optional<double> offsetFirst = offset ? offset(first.filePath) : std::optional<double>(0.0);
            const std::optional<double> offsetCopy = offset ? offset(copy.filePath) : std::optional<double>(0.0);
            if (!offsetFirst || !offsetCopy) {
                continue;
            }
            group.cueShiftMs[i] = *decoderShift + *offsetCopy - *offsetFirst;
        }
    }
}

std::optional<double> cueShiftBetween(const DuplicateGroup &group, size_t from, size_t to)
{
    if (group.cueShiftMs.empty()) {
        return 0.0;
    }
    if (from >= group.cueShiftMs.size() || to >= group.cueShiftMs.size()) {
        return std::nullopt;
    }
    const auto &a = group.cueShiftMs[from];
    const auto &b = group.cueShiftMs[to];
    if (!a || !b) {
        return std::nullopt;
    }
    const double shift = *b - *a;
    return std::abs(shift) < NegligibleCueShiftMs ? 0.0 : shift;
}

std::vector<CuePoint> shiftedCues(const std::vector<CuePoint> &cues, double shiftMs)
{
    if (std::abs(shiftMs) < NegligibleCueShiftMs) {
        return cues;
    }
    std::vector<CuePoint> moved = cues;
    for (auto &cue : moved) {
        cue.positionMs += shiftMs;
        if (cue.isLoop) {
            cue.loopEndMs += shiftMs;
        }
    }
    return moved;
}

MovedCues movedCues(const std::vector<CuePoint> &cues, double shiftMs, double targetSeconds)
{
    MovedCues result;
    if (std::abs(shiftMs) < NegligibleCueShiftMs) {
        result.cues = cues;
        return result;
    }
    const double lengthMs = targetSeconds > 0.0 ? targetSeconds * 1000.0 : 0.0;
    auto inside = [lengthMs](double ms) { return ms >= 0.0 && (lengthMs <= 0.0 || ms <= lengthMs); };
    for (CuePoint cue : cues) {
        cue.positionMs += shiftMs;
        if (cue.isLoop) {
            cue.loopEndMs += shiftMs;
        }
        // A cue moved off either end of the track marks a sound that copy
        // does not have. Written anyway it would be clamped or refused by
        // the player, so it is counted and said instead.
        if (!inside(cue.positionMs) || (cue.isLoop && !inside(cue.loopEndMs))) {
            ++result.leftOut;
            continue;
        }
        result.cues.push_back(cue);
    }
    return result;
}

}  // namespace seabass::domain
