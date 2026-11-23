// SPDX-FileCopyrightText: 2001-2025 Mixxx Development Team
// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-or-later

// Mixxx's ironing of a beat tracker's beats into a grid: BeatUtils
// (src/track/beatutils.cpp) and the constant grid BeatFactory makes from
// it (src/track/beatfactory.cpp), ported from Mixxx 2.5.3 (commit
// 93275786c517e0804951f7428a5cf045aa541b8d) for the phase 2 baseline of
// docs/track-analysis-plan.md. Mixxx is "Copyright (C) 2001-2025 Mixxx
// Development Team" and licensed under the GNU General Public License,
// version 2 or (at your option) any later version; so is this port.
//
// What changed in the port: Qt's containers and Mixxx's FramePos, Bpm and
// SampleRate types are plain doubles and std::vector here (an invalid Bpm,
// Mixxx's 0, is std::nullopt), the debug output is gone, and the BPM
// rounding can be switched off for the unrounded variant. The arithmetic
// and its order are Mixxx's. A Mixxx frame is one sample of the mono
// signal here.
//
// For the benchmark tool only; not part of the app.

#pragma once

#include <optional>
#include <vector>

namespace seabass::baseline::mixxx
{

struct ConstRegion
{
    double firstBeat = 0.0;   // frames
    double beatLength = 0.0;  // frames; 0 on the closing marker
};

// The stretches over which the beats keep one length within 25 ms (one
// beat allowed out, the first never, and their errors not adding up past
// 100 ms), each with its first beat and the mean beat length. The last
// entry is a marker of length 0 at the last beat.
std::vector<ConstRegion> retrieveConstRegions(const std::vector<double> &coarseBeats, double sampleRate);

// The longest region, joined with a region before and after it whose
// tempo and phase fit, as one tempo; rounded (roundBpmWithinRange) when
// `roundBpm`, otherwise that tempo as measured. *firstBeat, when given,
// is the joined stretch's first beat folded to within one beat of the
// track's start. nullopt when no region has any length, which Mixxx
// takes as "no tempo".
std::optional<double> makeConstBpm(const std::vector<ConstRegion> &constantRegions, double sampleRate,
                                   double *firstBeat, bool roundBpm = true);

// Mixxx's BPM snapping: the first of a whole BPM, a half BPM (under
// 85 BPM), a value on 2/3 (over 127 BPM), a third, a twelfth that lies
// strictly between min and max; otherwise the centre as it is.
std::optional<double> trySnap(double minBpm, double centerBpm, double maxBpm, double fraction);
double roundBpmWithinRange(double minBpm, double centerBpm, double maxBpm);

// The first beat moved by the mean offset of every coarse beat within
// 25 ms of the grid's phase.
double adjustPhase(double firstBeat, double bpm, double sampleRate, const std::vector<double> &beats);

// The ironed beats of a moving grid (Mixxx's beat map), region by
// region.
std::vector<double> getBeats(const std::vector<ConstRegion> &constantRegions);

// BeatFactory::makePreferredBeats with the fixed tempo assumption on,
// Mixxx's default: the anchor of a constant grid, the frame it sits on
// and its tempo. nullopt where Mixxx makes no grid.
struct ConstGrid
{
    double firstBeatFrame = 0.0;
    double bpm = 0.0;
};
std::optional<ConstGrid> makeConstGrid(const std::vector<double> &beats, double sampleRate, bool roundBpm = true);

}  // namespace seabass::baseline::mixxx
