// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <algorithm>
#include <cmath>

namespace seabass::domain
{

// How far apart two cues, one on each side of a sync, may be and still be
// the same cue: less than half a beat of the track's tempo. A tolerance is
// exclusive everywhere it is used: two cues exactly that far apart are two
// cues.
//
// A hot cue belongs to one beat or the other. A cue more than half a beat
// from another is nearer a different beat, so it is a different cue; and
// under the default quantize two cues within half a beat land on the same
// beat anyway. Finer differences are for the UI to show, not for the
// planner to settle. Half a beat is 234 ms at 128 BPM, 172 ms at 174.
//
// Measured on a real stick (WHALESHARK2, 2026-10-04), the 169 hot cues
// rekordbox and Engine both hold on the same pad were 150 at 0 ms, 2 at
// 1 ms (Engine stores sample offsets and rounds), 13 at exactly 52 ms
// (two MP3 tracks Engine's own older rekordbox import had shifted by 2304
// samples at 44.1 kHz, two MP3 frames of decoder delay; 48 ms at 48 kHz),
// none between 2 and 51 or 53 and 1000 ms, and 4 beyond a second
// (different cues). Memory cues: 15 at 0 ms, 1 at 1 ms. Every measured
// class is within half a beat at any tempo a DJ plays; the 52 ms decoder
// delay is within it below 577 BPM.
//
// The tolerance used to be a second (a beat at 60 BPM), which made two
// cues on neighbouring beats one cue.
inline double halfBeatMs(double bpm)
{
    return 30000.0 / bpm;
}

// Used when the two sides do not agree on a tempo, or either has none:
// above every measured rounding class (0, 1 and 52 ms) and under a 32nd
// note at 128 BPM (59 ms). Two cues 60 ms or more apart are then two
// cues, and the planner says that it could not use half a beat
// (SyncPlan::Reason::TempoUnsure) wherever that decided anything.
constexpr double CueFallbackToleranceMs = 60.0;

// Two tempos within 1% of each other are one tempo: readers round
// differently, and a beat grid nudged by a fraction of a BPM is the same
// grid.
constexpr double TempoAgreementFraction = 0.01;


struct CueTolerance
{
    double ms = CueFallbackToleranceMs;
    // True when both sides agree on a tempo and ms is half a beat of it.
    bool fromTempo = false;
    double bpm = 0.0;  // the agreed tempo, when fromTempo
    // When not fromTempo: the widest half beat either side's tempo could
    // mean. Two cues farther apart than ms but within this might still be
    // one cue.
    double unsureUpToMs = CueFallbackToleranceMs;
};

// A tempo a track can have. Outside this a reader misread or a
// placeholder is likelier than music, and half a beat of it would be no
// tolerance at all (a second at 30 BPM, the old one, is the most allowed).
constexpr double MinimumTempoBpm = 30.0;
constexpr double MaximumTempoBpm = 300.0;

inline bool isKnownTempo(double bpm)
{
    return std::isfinite(bpm) && bpm >= MinimumTempoBpm && bpm <= MaximumTempoBpm;
}

// When neither side has a tempo, the widest half beat that might apply:
// half a beat at the slowest tempo there is. A pair of cues within this but
// beyond the fallback could be one cue at some tempo, so it is not settled
// silently.
inline double unknownTempoHalfBeatMs()
{
    return halfBeatMs(MinimumTempoBpm);
}

inline CueTolerance cueToleranceFor(double bpmA, double bpmB)
{
    CueTolerance tolerance;
    const bool knownA = isKnownTempo(bpmA);
    const bool knownB = isKnownTempo(bpmB);
    if (knownA && knownB && std::abs(bpmA - bpmB) <= TempoAgreementFraction * std::max(bpmA, bpmB)) {
        tolerance.fromTempo = true;
        tolerance.bpm = (bpmA + bpmB) / 2.0;
        tolerance.ms = halfBeatMs(tolerance.bpm);
        tolerance.unsureUpToMs = tolerance.ms;
        return tolerance;
    }
    if (knownA || knownB) {
        const double slowest = knownA && knownB ? std::min(bpmA, bpmB) : (knownA ? bpmA : bpmB);
        tolerance.unsureUpToMs = std::max(CueFallbackToleranceMs, halfBeatMs(slowest));
    } else {
        tolerance.unsureUpToMs = unknownTempoHalfBeatMs();
    }
    return tolerance;
}

}  // namespace seabass::domain
