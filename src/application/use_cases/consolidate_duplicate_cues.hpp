// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

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
    std::vector<domain::ConsolidationPlan> execute(const std::vector<domain::Track> &tracks)
    {
        std::vector<domain::ConsolidationPlan> plans;
        for (const auto &group : domain::DuplicateTrackFinder::find(tracks)) {
            plans.push_back(domain::DuplicateCueConsolidator::plan(group));
        }
        return plans;
    }
};

}  // namespace seabass::application
