// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <utility>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// One stick track matched against a local-backup copy that has cues the
// stick is missing, paired with the full cue list writing this candidate
// would produce (the stick's own cues, kept exactly as they are, plus
// whichever of the local backup's cues fill a gap -- see
// LocalRestorePlanner::mergeCues() for the merge rule). mergedCues is the
// complete set to hand a CueWriter, not a diff: writeHotCues() replaces a
// track's entire cue set, so anything already on the stick must be
// resupplied alongside the additions or it would be wiped out.
struct RestoreCandidate
{
    Track stickTrack;
    Track localTrack;
    std::vector<CuePoint> mergedCues;
};

// Decides which title+artist/filename matches between a stick's tracks
// and a local cue backup are worth restoring, and what merging them would
// actually produce. Restoring only ever writes to the stick (never back to
// the local backup), and never overwrites a cue already on the stick --
// only ever adds ones the stick is missing:
//   - Hot cues are keyed by slot number (1-8): a local hot cue is added
//     only if the stick has no hot cue in that slot yet.
//   - Memory cues have no slot number, so two are treated as "the same"
//     cue if their positions are within PositionToleranceMs of each other;
//     a local memory cue is added only if no existing stick memory cue is
//     that close to it (avoids near-duplicate clutter from cues that are
//     really the same beat, off by a little due to source rounding).
// A candidate is only proposed when merging would actually add at least
// one cue -- a track already carrying everything the backup has to offer
// is left alone.
class LocalRestorePlanner
{
public:
    static constexpr double PositionToleranceMs = 500.0;

    static std::vector<CuePoint> mergeCues(const std::vector<CuePoint> &existing,
                                            const std::vector<CuePoint> &incoming);

    static std::vector<RestoreCandidate> plan(
        const std::vector<std::pair<const Track *, const Track *>> &matches);
};

}  // namespace seabass::domain
