// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <chrono>
#include <vector>

#include "domain/sync_planning.hpp"
#include "domain/track.hpp"

namespace seabass::application
{

// Read-only use case: match tracks across two catalog scans and decide
// what should happen to each pair's cues. Deliberately generic (tracksA/
// tracksB, not named after specific formats) -- callers run this once
// per pair of catalogs actually present on a stick (rekordbox<->Engine,
// rekordbox<->OneLibrary, Engine<->OneLibrary), see
// gui::SyncController::analyze(). Applying a plan is a separate step
// (see cli/), since only some directions are writable.
class SyncLibraries
{
public:
    std::vector<domain::SyncPlan> execute(const std::vector<domain::Track> &tracksA,
                                           const std::vector<domain::Track> &tracksB,
                                           std::chrono::system_clock::time_point mtimeA,
                                           std::chrono::system_clock::time_point mtimeB)
    {
        std::vector<domain::SyncPlan> plans;
        for (const auto &match : domain::TrackMatcher::match(tracksA, tracksB)) {
            plans.push_back(domain::SyncPlanner::plan(match, mtimeA, mtimeB));
        }
        return plans;
    }
};

}  // namespace seabass::application
