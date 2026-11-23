// SPDX-FileCopyrightText: 2001-2025 Mixxx Development Team
// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// Ported from Mixxx 2.5.3, src/track/beatutils.cpp and
// src/track/beatfactory.cpp; see mixxx_beatutils.hpp. Mixxx's own
// comments are kept where they explain the method.

#include "analysis_baseline/mixxx_beatutils.hpp"

#include <algorithm>
#include <cmath>

namespace seabass::baseline::mixxx
{

namespace
{

// When ironing the grid for long sequences of const tempo we use
// a 25 ms tolerance because this small of a difference is inaudible
// This is > 2 * 12 ms, the step width of the QM beat detector
constexpr double kMaxSecsPhaseError = 0.025;
// This is set to avoid to use a constant region during an offset shift.
// That happens for instance when the beat instrument changes.
constexpr double kMaxSecsPhaseErrorSum = 0.1;
constexpr int kMaxOutliersCount = 1;
constexpr int kMinRegionBeatCount = 16;

// Mixxx's Bpm::isValid: finite and above 0.
bool validBpm(double value)
{
    return std::isfinite(value) && value > 0.0;
}

}  // namespace

std::vector<ConstRegion> retrieveConstRegions(const std::vector<double> &coarseBeats, double sampleRate)
{
    if (coarseBeats.size() < 2) {
        // Cannot infer a tempo for less than 2 beats
        return {};
    }

    // The QM Beat detector has a step size of 512 frames @ 44100 Hz. This
    // means that single beats have a jitter of +- 12 ms around the actual
    // position. This function irons these adjustment beats by adjusting
    // every beat to the average of a likely constant region.
    const double maxPhaseError = kMaxSecsPhaseError * sampleRate;
    const double maxPhaseErrorSum = kMaxSecsPhaseErrorSum * sampleRate;
    const int size = static_cast<int>(coarseBeats.size());
    int leftIndex = 0;
    int rightIndex = size - 1;

    std::vector<ConstRegion> constantRegions;
    while (leftIndex < size - 1) {
        const double meanBeatLength =
            (coarseBeats[rightIndex] - coarseBeats[leftIndex]) / (rightIndex - leftIndex);
        int outliersCount = 0;
        double ironedBeat = coarseBeats[leftIndex];
        double phaseErrorSum = 0;
        int i = leftIndex + 1;
        for (; i <= rightIndex; ++i) {
            ironedBeat += meanBeatLength;
            const double phaseError = ironedBeat - coarseBeats[i];
            phaseErrorSum += phaseError;
            if (std::fabs(phaseError) > maxPhaseError) {
                outliersCount++;
                if (outliersCount > kMaxOutliersCount || i == leftIndex + 1) {
                    // The first beat must not be an outlier: the region
                    // is not constant.
                    break;
                }
            }
            if (std::fabs(phaseErrorSum) > maxPhaseErrorSum) {
                // We drift away in one direction: the mean beat length is
                // not optimal.
                break;
            }
        }
        if (i > rightIndex) {
            // Verify that the first and the last beat are not correction
            // beats in the same direction, which would bend the mean beat
            // length away from the optimum.
            double regionBorderError = 0;
            if (rightIndex > leftIndex + 2) {
                const double firstBeatLength = coarseBeats[leftIndex + 1] - coarseBeats[leftIndex];
                const double lastBeatLength = coarseBeats[rightIndex] - coarseBeats[rightIndex - 1];
                regionBorderError = std::fabs(firstBeatLength + lastBeatLength - (2 * meanBeatLength));
            }
            if (regionBorderError < maxPhaseError / 2) {
                // A constant enough region: kept, and the next one starts
                // at its last beat.
                constantRegions.push_back({coarseBeats[leftIndex], meanBeatLength});
                leftIndex = rightIndex;
                rightIndex = size - 1;
                continue;
            }
        }
        // Try a region one beat shorter.
        rightIndex--;
    }

    // A final region with zero length marks the end.
    constantRegions.push_back({coarseBeats.back(), 0});
    return constantRegions;
}

std::optional<double> makeConstBpm(const std::vector<ConstRegion> &constantRegions, double sampleRate,
                                   double *pFirstBeat, bool roundBpm)
{
    // We assume here the track was recorded with an unhearable static
    // metronome, likely at a full BPM. The track may have intros, outros
    // and bridges without detectable beats, regions with other rhythm
    // instruments that shift the onsets, or break beats that make the
    // detector hook onto an integer fraction of the metronome. This finds
    // the metronome and a phase offset, starting from the longest region.
    const int count = static_cast<int>(constantRegions.size());
    int midRegionIndex = 0;
    double longestRegionLength = 0;
    double longestRegionBeatLength = 0;
    for (int i = 0; i < count - 1; ++i) {
        const double length = constantRegions[i + 1].firstBeat - constantRegions[i].firstBeat;
        if (length > longestRegionLength) {
            longestRegionLength = length;
            longestRegionBeatLength = constantRegions[i].beatLength;
            midRegionIndex = i;
        }
    }

    if (longestRegionLength == 0) {
        // Could not infer a tempo.
        return std::nullopt;
    }

    int longestRegionNumberOfBeats = static_cast<int>((longestRegionLength / longestRegionBeatLength) + 0.5);
    double longestRegionBeatLengthMin =
        longestRegionBeatLength - ((kMaxSecsPhaseError * sampleRate) / longestRegionNumberOfBeats);
    double longestRegionBeatLengthMax =
        longestRegionBeatLength + ((kMaxSecsPhaseError * sampleRate) / longestRegionNumberOfBeats);

    int startRegionIndex = midRegionIndex;

    // Find a region at the beginning of the track with a similar tempo and
    // phase.
    for (int i = 0; i < midRegionIndex; ++i) {
        const double length = constantRegions[i + 1].firstBeat - constantRegions[i].firstBeat;
        const int numberOfBeats = static_cast<int>((length / constantRegions[i].beatLength) + 0.5);
        if (numberOfBeats < kMinRegionBeatCount) {
            // Short regions are too unstable.
            continue;
        }
        const double thisRegionBeatLengthMin =
            constantRegions[i].beatLength - ((kMaxSecsPhaseError * sampleRate) / numberOfBeats);
        const double thisRegionBeatLengthMax =
            constantRegions[i].beatLength + ((kMaxSecsPhaseError * sampleRate) / numberOfBeats);
        // Is the tempo of the longest region inside the rounding range of
        // this one?
        if (longestRegionBeatLength > thisRegionBeatLengthMin && longestRegionBeatLength < thisRegionBeatLengthMax) {
            // Now check if both regions are at the same phase.
            const double newLongestRegionLength =
                constantRegions[midRegionIndex + 1].firstBeat - constantRegions[i].firstBeat;

            const double beatLengthMin = std::max(longestRegionBeatLengthMin, thisRegionBeatLengthMin);
            const double beatLengthMax = std::min(longestRegionBeatLengthMax, thisRegionBeatLengthMax);

            const int maxNumberOfBeats = static_cast<int>(std::round(newLongestRegionLength / beatLengthMin));
            const int minNumberOfBeats = static_cast<int>(std::round(newLongestRegionLength / beatLengthMax));

            if (minNumberOfBeats != maxNumberOfBeats) {
                // Ambiguous number of beats: find a closer region.
                continue;
            }
            const int numberOfBeatsJoined = minNumberOfBeats;
            const double newBeatLength = newLongestRegionLength / numberOfBeatsJoined;
            if (newBeatLength > longestRegionBeatLengthMin && newBeatLength < longestRegionBeatLengthMax) {
                longestRegionLength = newLongestRegionLength;
                longestRegionBeatLength = newBeatLength;
                longestRegionNumberOfBeats = numberOfBeatsJoined;
                longestRegionBeatLengthMin =
                    longestRegionBeatLength - ((kMaxSecsPhaseError * sampleRate) / longestRegionNumberOfBeats);
                longestRegionBeatLengthMax =
                    longestRegionBeatLength + ((kMaxSecsPhaseError * sampleRate) / longestRegionNumberOfBeats);
                startRegionIndex = i;
                break;
            }
        }
    }

    // Find a region at the end of the track with a similar tempo and
    // phase.
    for (int i = count - 2; i > midRegionIndex; --i) {
        const double length = constantRegions[i + 1].firstBeat - constantRegions[i].firstBeat;
        const int numberOfBeats = static_cast<int>((length / constantRegions[i].beatLength) + 0.5);
        if (numberOfBeats < kMinRegionBeatCount) {
            continue;
        }
        const double thisRegionBeatLengthMin =
            constantRegions[i].beatLength - ((kMaxSecsPhaseError * sampleRate) / numberOfBeats);
        const double thisRegionBeatLengthMax =
            constantRegions[i].beatLength + ((kMaxSecsPhaseError * sampleRate) / numberOfBeats);
        if (longestRegionBeatLength > thisRegionBeatLengthMin && longestRegionBeatLength < thisRegionBeatLengthMax) {
            // Now check if both regions are at the same phase.
            const double newLongestRegionLength =
                constantRegions[i + 1].firstBeat - constantRegions[startRegionIndex].firstBeat;

            const double minBeatLength = std::max(longestRegionBeatLengthMin, thisRegionBeatLengthMin);
            const double maxBeatLength = std::min(longestRegionBeatLengthMax, thisRegionBeatLengthMax);

            const int maxNumberOfBeats = static_cast<int>(std::round(newLongestRegionLength / minBeatLength));
            const int minNumberOfBeats = static_cast<int>(std::round(newLongestRegionLength / maxBeatLength));

            if (minNumberOfBeats != maxNumberOfBeats) {
                // Ambiguous number of beats: find a closer region.
                continue;
            }
            const int numberOfBeatsJoined = minNumberOfBeats;
            const double newBeatLength = newLongestRegionLength / numberOfBeatsJoined;
            if (newBeatLength > longestRegionBeatLengthMin && newBeatLength < longestRegionBeatLengthMax) {
                longestRegionLength = newLongestRegionLength;
                longestRegionBeatLength = newBeatLength;
                longestRegionNumberOfBeats = numberOfBeatsJoined;
                break;
            }
        }
    }

    longestRegionBeatLengthMin =
        longestRegionBeatLength - ((kMaxSecsPhaseError * sampleRate) / longestRegionNumberOfBeats);
    longestRegionBeatLengthMax =
        longestRegionBeatLength + ((kMaxSecsPhaseError * sampleRate) / longestRegionNumberOfBeats);

    // One constant region from the first beat of the first region to the
    // last beat of the last region.
    const double minRoundBpm = 60.0 * sampleRate / longestRegionBeatLengthMax;
    const double maxRoundBpm = 60.0 * sampleRate / longestRegionBeatLengthMin;
    const double centerBpm = 60.0 * sampleRate / longestRegionBeatLength;

    const double bpm = roundBpm ? roundBpmWithinRange(minRoundBpm, centerBpm, maxRoundBpm) : centerBpm;

    if (pFirstBeat) {
        // Move the first beat as close to the start of the track as we
        // can. This is a constant grid, so the first beat only sets the
        // anchor.
        const double roundedBeatLength = 60.0 * sampleRate / bpm;
        *pFirstBeat = std::fmod(constantRegions[startRegionIndex].firstBeat, roundedBeatLength);
    }
    if (!validBpm(bpm)) {
        return std::nullopt;
    }
    return bpm;
}

std::optional<double> trySnap(double minBpm, double centerBpm, double maxBpm, double fraction)
{
    const double snapBpm = std::round(centerBpm * fraction) / fraction;
    if (snapBpm > minBpm && snapBpm < maxBpm) {
        return snapBpm;
    }
    return std::nullopt;
}

double roundBpmWithinRange(double minBpm, double centerBpm, double maxBpm)
{
    // First try to snap to a full integer BPM.
    if (const auto snapBpm = trySnap(minBpm, centerBpm, maxBpm, 1.0)) {
        return *snapBpm;
    }

    // 0.5 BPM are only reasonable if the double value is not insane,
    // else other factors below are more typical.
    if (centerBpm < 85.0) {
        if (const auto snapBpm = trySnap(minBpm, centerBpm, maxBpm, 2.0)) {
            return *snapBpm;
        }
    }

    if (centerBpm > 127.0) {
        // Optimise for 2/3, going down to 85.
        if (const auto snapBpm = trySnap(minBpm, centerBpm, maxBpm, 2.0 / 3.0)) {
            return *snapBpm;
        }
    }

    // A third of a BPM covers all sorts of 3/2 and 3/4 multipliers.
    if (const auto snapBpm = trySnap(minBpm, centerBpm, maxBpm, 3.0)) {
        return *snapBpm;
    }

    // A twelfth covers all other sorts of typical multipliers.
    if (const auto snapBpm = trySnap(minBpm, centerBpm, maxBpm, 12.0)) {
        return *snapBpm;
    }

    // Else give up and use the original BPM value.
    return centerBpm;
}

std::vector<double> getBeats(const std::vector<ConstRegion> &constantRegions)
{
    std::vector<double> beats;
    for (std::size_t i = 0; i + 1 < constantRegions.size(); ++i) {
        double beat = constantRegions[i].firstBeat;
        constexpr double epsilon = 100;  // Protection against tiny beats due to rounding.
        while (beat < constantRegions[i + 1].firstBeat - epsilon) {
            beats.push_back(beat);
            beat += constantRegions[i].beatLength;
        }
    }
    if (!constantRegions.empty()) {
        beats.push_back(constantRegions.back().firstBeat);
    }
    return beats;
}

double adjustPhase(double firstBeat, double bpm, double sampleRate, const std::vector<double> &beats)
{
    const double beatLength = 60 * sampleRate / bpm;
    const double startOffset = std::fmod(firstBeat, beatLength);
    double offsetAdjust = 0;
    double offsetAdjustCount = 0;
    for (const double beat : beats) {
        double offset = std::fmod(beat - startOffset, beatLength);
        if (offset > beatLength / 2) {
            offset -= beatLength;
        }
        // Mixxx writes abs() on a double here, which with its headers is
        // the floating point overload.
        if (std::abs(offset) < (kMaxSecsPhaseError * sampleRate)) {
            offsetAdjust += offset;
            offsetAdjustCount++;
        }
    }
    // Mixxx divides without a guard; with no beat in reach the result is
    // not a number, and so is the grid's anchor. That case is left to the
    // caller (makeConstGrid) rather than changed here.
    offsetAdjust /= offsetAdjustCount;
    return firstBeat + offsetAdjust;
}

std::optional<ConstGrid> makeConstGrid(const std::vector<double> &beats, double sampleRate, bool roundBpm)
{
    const std::vector<ConstRegion> constantRegions = retrieveConstRegions(beats, sampleRate);
    if (constantRegions.empty()) {
        return std::nullopt;
    }
    double firstBeat = 0.0;
    const std::optional<double> bpm = makeConstBpm(constantRegions, sampleRate, &firstBeat, roundBpm);
    if (!bpm) {
        // Mixxx would call Beats::fromConstTempo with an invalid tempo,
        // which refuses it: no grid.
        return std::nullopt;
    }
    firstBeat = adjustPhase(firstBeat, *bpm, sampleRate, beats);
    if (!std::isfinite(firstBeat)) {
        return std::nullopt;
    }
    // FramePos::toNearestFrameBoundary.
    return ConstGrid{std::round(firstBeat), *bpm};
}

}  // namespace seabass::baseline::mixxx
