// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/local_restore.hpp"

#include <cmath>

#include "domain/cue_tolerance.hpp"

namespace seabass::domain
{

std::vector<CuePoint> LocalRestorePlanner::mergeCues(const std::vector<CuePoint> &existing,
                                                       const std::vector<CuePoint> &incoming, double toleranceMs)
{
    std::vector<CuePoint> merged = existing;

    for (const auto &candidate : incoming) {
        if (candidate.kind == CuePoint::Kind::Hot) {
            bool slotTaken = false;
            for (const auto &have : existing) {
                if (have.kind == CuePoint::Kind::Hot && have.hotCueNumber == candidate.hotCueNumber) {
                    slotTaken = true;
                    break;
                }
            }
            if (!slotTaken) {
                merged.push_back(candidate);
            }
        } else {
            bool nearExisting = false;
            for (const auto &have : existing) {
                // Inclusive, as it always was here. A loop is only ever
                // the same as a loop with the same in and out point: a
                // memory loop at a cue's place used to be dropped as that
                // cue.
                const bool samePlace = have.isLoop == candidate.isLoop
                    && std::abs(have.positionMs - candidate.positionMs) <= toleranceMs
                    && (!have.isLoop || std::abs(have.loopEndMs - candidate.loopEndMs) <= toleranceMs);
                if (have.kind == CuePoint::Kind::Memory && samePlace) {
                    nearExisting = true;
                    break;
                }
            }
            if (!nearExisting) {
                merged.push_back(candidate);
            }
        }
    }

    return merged;
}

std::vector<RestoreCandidate> LocalRestorePlanner::plan(
    const std::vector<std::pair<const Track *, const Track *>> &matches)
{
    std::vector<RestoreCandidate> candidates;
    for (const auto &[stickTrack, localTrack] : matches) {
        // matchTracks() itself tolerates an unreadable (zero) duration on
        // either side by skipping the duration check entirely -- a
        // deliberate looseness tuned for its other caller (same-stick
        // rekordbox<->Engine sync, where one side's duration reliably
        // fails to read far more often than titles genuinely collide).
        // A local cue backup crosses stick boundaries, where a title+
        // artist collision between two actually-different tracks (a
        // remix, a radio edit, a re-rip) is a real risk worth guarding
        // against even at the cost of an occasional missed match when a
        // duration is unreadable -- so here, unlike matchTracks(), a
        // known duration on both sides that agrees to the nearest second
        // is required, not just preferred.
        if (stickTrack->durationSeconds <= 0.0 || localTrack->durationSeconds <= 0.0 ||
            std::lround(stickTrack->durationSeconds) != std::lround(localTrack->durationSeconds)) {
            continue;
        }
        auto merged = mergeCues(stickTrack->cues, localTrack->cues,
                                cueToleranceMsFor(stickTrack->bpm, localTrack->bpm));
        if (merged.size() > stickTrack->cues.size()) {
            candidates.push_back({*stickTrack, *localTrack, std::move(merged)});
        }
    }
    return candidates;
}

}  // namespace seabass::domain
