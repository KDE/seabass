// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/library_consistency.hpp"

#include <set>

#include "domain/duplicate_cue_consolidation.hpp"

namespace seabass::domain
{

std::vector<LibraryConsistencyIssue> LibraryConsistencyChecker::check(const std::vector<Track> &healthyTracks,
                                                                        const std::vector<Track> &brokenTracks)
{
    std::set<std::string> brokenIds;
    for (const auto &t : brokenTracks) {
        brokenIds.insert(t.sourceId);
    }

    std::vector<Track> all = healthyTracks;
    all.insert(all.end(), brokenTracks.begin(), brokenTracks.end());
    std::vector<DuplicateGroup> groups = DuplicateTrackFinder::find(all);

    std::vector<LibraryConsistencyIssue> issues;
    std::set<std::string> handledBrokenIds;

    for (auto &group : groups) {
        std::vector<Track> brokenInGroup;
        std::vector<Track> healthyInGroup;
        for (const auto &t : group.tracks) {
            (brokenIds.count(t.sourceId) ? brokenInGroup : healthyInGroup).push_back(t);
        }
        if (brokenInGroup.empty()) {
            continue;  // no broken tracks here, irrelevant to this checker
        }
        for (const auto &t : brokenInGroup) {
            handledBrokenIds.insert(t.sourceId);
        }

        if (healthyInGroup.empty()) {
            // Every copy in this group is broken, nothing to survive
            // onto, even though DuplicateTrackFinder still recognized
            // them as copies of the same song.
            LibraryConsistencyIssue issue;
            issue.kind = LibraryConsistencyIssue::Kind::Missing;
            issue.brokenGroup = std::move(brokenInGroup);
            issues.push_back(std::move(issue));
            continue;
        }

        // More than one healthy row matching the same broken row(s)
        // would itself be a duplicate Clean Up should have already
        // consolidated. Picking the first is a reasonable, clearly-
        // logged simplification rather than fabricating a merge across
        // multiple healthy candidates.
        Track survivor = healthyInGroup.front();

        ConsolidationPlan cuePlan = DuplicateCueConsolidator::plan(group);
        LibraryConsistencyIssue issue;
        issue.brokenGroup = std::move(brokenInGroup);
        issue.survivor = survivor;
        if (cuePlan.kind == ConsolidationPlan::Kind::Conflict) {
            issue.kind = LibraryConsistencyIssue::Kind::Conflict;
        } else {
            issue.kind = LibraryConsistencyIssue::Kind::Repairable;
            // Only a real write if the authoritative cue set turned out
            // to live on a broken sibling rather than the survivor
            // itself. When the survivor is already the (or an
            // equally-)cued copy, there's nothing to merge onto it.
            if (cuePlan.kind == ConsolidationPlan::Kind::Unambiguous && cuePlan.source &&
                cuePlan.source->sourceId != survivor.sourceId) {
                issue.survivorCues = cuePlan.source->cues;
            }
        }
        issues.push_back(std::move(issue));
    }

    // Any broken track DuplicateTrackFinder didn't group with anything
    // at all (no filename or title+artist+duration match, healthy or
    // broken), a lone broken row, no candidate anywhere in this
    // catalog.
    for (const auto &t : brokenTracks) {
        if (!handledBrokenIds.count(t.sourceId)) {
            LibraryConsistencyIssue issue;
            issue.kind = LibraryConsistencyIssue::Kind::Missing;
            issue.brokenGroup = {t};
            issues.push_back(std::move(issue));
        }
    }

    return issues;
}

}  // namespace seabass::domain
