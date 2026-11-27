// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/cue_beat_alignment.hpp"

#include <algorithm>
#include <cmath>

namespace seabass::domain
{

namespace
{

// Two offsets are one offset when they are this close: a cue cut to whole
// milliseconds against a grid kept in samples.
constexpr double SameOffsetWithinMs = 1.5;

double nearestBeat(const std::vector<double> &beats, double ms)
{
    const auto after = std::lower_bound(beats.begin(), beats.end(), ms);
    if (after == beats.end()) {
        return beats.back();
    }
    if (after == beats.begin()) {
        return *after;
    }
    const double before = *(after - 1);
    return ms - before <= *after - ms ? before : *after;
}

bool counts(const CueBeatDistance &point, double toleranceMs)
{
    const double off = std::abs(point.offMs);
    // Never onto the very start of the track: Engine reads a main cue at
    // 0 as none, so a cue moved there would be lost.
    return off > CueOnBeatWithinMs && off <= toleranceMs && point.beatMs > 0.0;
}

}  // namespace

CueBeatAlignment measureCuesAgainstBeats(const std::vector<CuePoint> &cues, const std::vector<double> &beatsMs)
{
    CueBeatAlignment alignment;
    if (beatsMs.size() < 2) {
        return alignment;
    }
    const auto measure = [&](std::size_t index, bool loopEnd, double ms) {
        const double beat = nearestBeat(beatsMs, ms);
        alignment.points.push_back({index, loopEnd, ms, beat, ms - beat});
    };
    for (std::size_t i = 0; i < cues.size(); ++i) {
        measure(i, false, cues[i].positionMs);
        // A loop's end is measured only where it has a beat of its own:
        // a loop shorter than a beat ends nearest the beat it starts on
        // (or the next), and moving its end there would close it up or
        // stretch it to a whole beat. Such a loop keeps its length and
        // goes with its start.
        if (cues[i].isLoop && cues[i].loopEndMs > cues[i].positionMs) {
            const double startBeat = nearestBeat(beatsMs, cues[i].positionMs);
            const double endBeat = nearestBeat(beatsMs, cues[i].loopEndMs);
            const auto next = std::upper_bound(beatsMs.begin(), beatsMs.end(), startBeat);
            const bool ownBeat = endBeat > startBeat && (next == beatsMs.end() || endBeat > *next
                                                         || cues[i].loopEndMs - cues[i].positionMs >= 0.75 * (*next - startBeat));
            if (ownBeat) {
                measure(i, true, cues[i].loopEndMs);
            }
        }
    }
    // The starts that are off their beat: all by one amount?
    int off = 0;
    double least = 0.0;
    double most = 0.0;
    for (const CueBeatDistance &point : alignment.points) {
        if (point.loopEnd || std::abs(point.offMs) <= CueOnBeatWithinMs) {
            continue;
        }
        least = off == 0 ? point.offMs : std::min(least, point.offMs);
        most = off == 0 ? point.offMs : std::max(most, point.offMs);
        ++off;
    }
    alignment.sameOffset = off >= 2 && most - least <= SameOffsetWithinMs;
    return alignment;
}

std::vector<CueBeatDistance> offBeatWithin(const CueBeatAlignment &alignment, double toleranceMs)
{
    std::vector<CueBeatDistance> listed;
    for (const CueBeatDistance &point : alignment.points) {
        if (counts(point, toleranceMs)) {
            listed.push_back(point);
        }
    }
    return listed;
}

std::vector<CuePoint> cuesAlignedToBeats(std::vector<CuePoint> cues, const CueBeatAlignment &alignment, double toleranceMs)
{
    const std::vector<CuePoint> original = cues;
    // By cue: where its start goes, and where its end does.
    std::vector<double> startMove(cues.size(), 0.0);
    std::vector<bool> endOnItsOwn(cues.size(), false);
    for (const CueBeatDistance &point : alignment.points) {
        if (point.cueIndex >= cues.size() || !counts(point, toleranceMs)) {
            continue;
        }
        if (point.loopEnd) {
            cues[point.cueIndex].loopEndMs = std::max(0.0, point.beatMs);
            endOnItsOwn[point.cueIndex] = true;
        } else {
            startMove[point.cueIndex] = point.beatMs - point.positionMs;
            cues[point.cueIndex].positionMs = std::max(0.0, point.beatMs);
        }
    }
    for (std::size_t i = 0; i < cues.size(); ++i) {
        if (cues[i].isLoop && startMove[i] != 0.0 && !endOnItsOwn[i] && cues[i].loopEndMs > 0.0) {
            cues[i].loopEndMs = std::max(0.0, cues[i].loopEndMs + startMove[i]);
        }
    }
    // Never a loop closed up or turned round: such a loop goes back to
    // where it was, both ends.
    for (std::size_t i = 0; i < cues.size(); ++i) {
        if (cues[i].isLoop && cues[i].loopEndMs <= cues[i].positionMs) {
            cues[i].positionMs = original[i].positionMs;
            cues[i].loopEndMs = original[i].loopEndMs;
        }
    }
    return cues;
}

}  // namespace seabass::domain
