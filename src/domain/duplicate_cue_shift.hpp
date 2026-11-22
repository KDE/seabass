// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "domain/audio_alignment_probe.hpp"
#include "domain/duplicate_cue_consolidation.hpp"
#include "domain/track.hpp"

namespace seabass::domain
{

// Cues travel between copies of one song in two places, Cues on
// Duplicate Copies and Clean Up, and both compare copies' cue sets before
// deciding anything. The copies are often not sample-aligned (see
// audio_alignment_probe.hpp for what was measured), so every cue that
// crosses from one copy to another is moved by the shift between the
// two, and every comparison is made once both sets are on one copy's
// timeline. This is the step that measures those shifts and the
// arithmetic that applies them.

// A shift smaller than this is no shift: well under what anyone hears,
// and under the rounding of the positions themselves.
inline constexpr double NegligibleCueShiftMs = 1.0;

// For one file: rekordbox's position minus the decoder's for the same
// instant, in ms, or nullopt when it cannot be known. What turns a shift
// the decoder measured into one on the timeline cues are held on (see
// infrastructure/audio/playback_timeline.hpp, cueTimelineOffsetMs()).
using CueTimelineOffset = std::function<std::optional<double>(const std::string &absoluteFilePath)>;

// True when some copy in the group carries a cue, on its Track or in any
// of its catalog rows. A group without one has nothing to move, and is
// not measured.
bool groupCarriesCues(const DuplicateGroup &group);

// How many pairs measureCueShifts() will hand the probe for these
// groups: one per copy after the first, in each group that carries cues.
// For a progress bar, counted before any of them is measured.
size_t cueShiftPairsToMeasure(const std::vector<DuplicateGroup> &groups);

// Fills each group's cueShiftMs: every copy against the first, so the
// shift between any two copies is the difference of theirs. A group with
// no cues is left unmeasured (empty).
//
// The shift on the cue timeline is the decoder's shift plus offset(b)
// minus offset(a). A copy at the same path as the first, or that the probe
// knows holds the same bytes (AudioAlignmentProbe::sameContent()), is 0
// with no measuring and no offsets: those are equal for equal bytes even
// when neither can be read. Without a probe, or when the probe or either
// offset has no answer, that copy's shift is unknown, never 0.
//
// `measured` is called once per pair handed to the probe, for progress.
// Cancelling is the probe's business (it stops answering), and leaves
// unknowns, which no caller turns into a cue movement.
void measureCueShifts(std::vector<DuplicateGroup> &groups, AudioAlignmentProbe *probe,
                      const CueTimelineOffset &offset, const std::function<void()> &measured = {});

// What to add to a cue of copy `from` to put it on copy `to`'s timeline:
// shift(to) - shift(from), 0 when that is under NegligibleCueShiftMs,
// nullopt when either is unknown. A group not measured (empty
// cueShiftMs) answers 0: see DuplicateGroup.
std::optional<double> cueShiftBetween(const DuplicateGroup &group, size_t from, size_t to);

// Every position (and loop end) moved by `shiftMs`, nothing left out:
// for comparing two copies' cue sets on one timeline, never for writing.
std::vector<CuePoint> shiftedCues(const std::vector<CuePoint> &cues, double shiftMs);

// Cues as they are to be written onto another copy: moved by `shiftMs`,
// without any that would land before 0 or past the target's length
// (both ends of a loop must land inside it). `targetSeconds` of 0 is a
// length not known, and only the start is checked then. A shift under
// NegligibleCueShiftMs moves nothing and leaves nothing out.
struct MovedCues
{
    std::vector<CuePoint> cues;
    int leftOut = 0;
};
MovedCues movedCues(const std::vector<CuePoint> &cues, double shiftMs, double targetSeconds);

}  // namespace seabass::domain
