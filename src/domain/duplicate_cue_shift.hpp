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

// How many pairs measureCueShifts() is expected to hand the probe that
// the probe cannot answer at once (AudioAlignmentProbe::answersAtOnce()):
// one per copy after the first that is another file, in each group that
// carries cues, less those already known. For a progress bar, counted
// before any of them is measured; more are asked when a copy cannot be
// read and another takes its place as the reference.
size_t cueShiftPairsToMeasure(const std::vector<DuplicateGroup> &groups, AudioAlignmentProbe *probe = nullptr);

// Fills each group's cue shifts and marks it measured. A group with no
// cues is left unmeasured: nothing in it can move.
//
//   * One file listed twice, and copies the probe knows hold the same
//     bytes (AudioAlignmentProbe::sameContent()), line up at 0 with no
//     measuring and no offsets: those are equal for equal bytes even when
//     neither can be read. Always, whatever else in the group is unknown.
//   * Every other copy is measured against a reference: the first copy
//     that can be, in group order. When the reference turns out to be
//     unreadable, or its offset unknowable, the next copy takes its place.
//     A copy that can be measured against nothing stays unknown.
//
// The shift on the cue timeline is the decoder's shift plus offset(b)
// minus offset(a). Without a probe, or when the probe or either offset
// has no answer, the shift is unknown, never 0.
//
// `measured` is called once per pair handed to the probe's align(), with
// whether it cost a decode (false when the probe answered at once), for
// progress. Cancelling is the probe's business (it stops answering), and
// leaves unknowns, which no caller turns into a cue movement.
void measureCueShifts(std::vector<DuplicateGroup> &groups, AudioAlignmentProbe *probe,
                      const CueTimelineOffset &offset, const std::function<void(bool costly)> &measured = {});

// What to add to a cue of copy `from` to put it on copy `to`'s timeline:
// 0 for one copy or one file listed twice; otherwise, in a measured group,
// shift(to) - shift(from) when both are known against one anchor, 0 when
// that is under NegligibleCueShiftMs. nullopt for anything else, a group
// not measured included.
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
