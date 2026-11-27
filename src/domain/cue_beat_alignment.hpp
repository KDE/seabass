// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// Cue points that do not sit on the beat grid (Maintain, "Align Cue
// Points"): how far each cue of a track is from the nearest beat of the
// grid of the SAME library, on one timeline. Never across libraries: the
// same MP3 is stored 26 or 52 ms apart in rekordbox and in Engine, so a
// cue of one against the grid of the other is off by that much and means
// nothing.
//
// Measured once, for every cue; what counts as "off the beat" is the
// caller's tolerance, applied afterwards (offBeatWithin), so a page can
// move its tolerance without measuring again.

// A cue that is this close to a beat is on it: positions are stored as
// whole milliseconds (rekordbox cuts them down) and as samples, and an
// Engine row's cues of an MP3 are set against its grid through the file's
// lead-in, which Seabass knows to a little over a millisecond. Less than
// this is how the number was written, not where the cue was put.
inline constexpr double CueOnBeatWithinMs = 1.5;

// One point of a cue measured against the grid: its start, or the end of
// a loop.
struct CueBeatDistance
{
    // The cue's place in the list measured.
    std::size_t cueIndex = 0;
    bool loopEnd = false;
    double positionMs = 0.0;
    // The nearest beat, and the cue's place less the beat's: negative
    // when the cue comes before its beat.
    double beatMs = 0.0;
    double offMs = 0.0;
};

struct CueBeatAlignment
{
    // Every point measured, in the cues' order (a loop's start, then its
    // end). Empty when the grid has fewer than two beats: nothing can be
    // on or off it.
    std::vector<CueBeatDistance> points;
    // Every cue start that is off its beat is off by the same amount, to
    // the millisecond and a half, and there are at least two: the grid is
    // more likely shifted than the cues misplaced, and moving the cues
    // onto it would move them all off the music by that much.
    bool sameOffset = false;
};

// `beatsMs` in order, on the cues' own timeline. A cue before the first
// beat or after the last is measured against that beat.
CueBeatAlignment measureCuesAgainstBeats(const std::vector<CuePoint> &cues, const std::vector<double> &beatsMs);

// The points more than CueOnBeatWithinMs and at most `toleranceMs` from
// their beat: the ones a page lists, and aligning moves. One further off
// than the tolerance was put there on purpose (an off-beat cue, a vocal),
// and is left alone. So is one whose beat is the very start of the track:
// Engine reads a main cue at 0 as none.
std::vector<CueBeatDistance> offBeatWithin(const CueBeatAlignment &alignment, double toleranceMs);

// `cues` with every such point moved onto its beat. A loop whose start
// moves and whose end is not itself within the tolerance keeps its
// length: the end moves by the start's amount. Positions never below 0.
std::vector<CuePoint> cuesAlignedToBeats(std::vector<CuePoint> cues, const CueBeatAlignment &alignment, double toleranceMs);

}  // namespace seabass::domain
