// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
#include <vector>

#include "domain/audio_content_probe.hpp"
#include "domain/duplicate_cue_consolidation.hpp"
#include "domain/track.hpp"

namespace seabass::application
{

// Read-only use case: find duplicate tracks within one library scan and
// decide what (if anything) could be consolidated. Applying a plan is a
// separate step (see cli/, which is the only layer that knows whether a
// CueWriter exists for the format being scanned).
class ConsolidateDuplicateCues
{
public:
    // `probe` is passed straight through to DuplicateTrackFinder, and
    // exists so that Match Duplicate Cues and Clean Up Duplicates group
    // by exactly the same rule. They read the same stick and both say
    // "duplicate", so a pair that one of them groups and the other does
    // not is not a preference, it is two pages disagreeing about what is
    // on the stick. Null (the CLI, which has no Preferences to read)
    // compares stored lengths alone.
    //
    // `lineUp` runs between grouping and planning, and is where the
    // caller fills in each group's cue shifts (domain/duplicate_cue_shift
    // .hpp): the plan compares and copies cues across copies, so it must
    // see them. Without one every group is planned as if its copies were
    // sample-aligned, which only a caller with no audio files to measure
    // (a test) has any business doing.
    std::vector<domain::ConsolidationPlan> execute(
        const std::vector<domain::Track> &tracks, domain::AudioContentProbe *probe = nullptr,
        const std::function<void(std::vector<domain::DuplicateGroup> &)> &lineUp = {})
    {
        std::vector<domain::DuplicateGroup> groups = domain::DuplicateTrackFinder::find(tracks, probe);
        if (lineUp) {
            lineUp(groups);
        }
        std::vector<domain::ConsolidationPlan> plans;
        for (const auto &group : groups) {
            plans.push_back(domain::DuplicateCueConsolidator::plan(group));
        }
        return plans;
    }
};

}  // namespace seabass::application
