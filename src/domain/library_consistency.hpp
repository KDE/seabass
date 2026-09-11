// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// One row in a single catalog (rekordbox, Engine, or OneLibrary) whose
// backing audio file no longer exists on disk, classified by whether a
// healthy same-catalog duplicate can absorb it. Never crosses catalogs,
// see LibraryConsistencyChecker::check()'s own comment.
struct LibraryConsistencyIssue
{
    enum class Kind {
        // A healthy sibling exists, and any cues the broken row(s) carry
        // either already match the survivor's or can be added to it
        // unambiguously, safe to auto-repair: write survivorCues onto
        // the survivor (if non-empty) and remove every track in
        // brokenGroup.
        Repairable,
        // A healthy sibling exists, but it and a broken row have
        // genuinely different cues, same "never guess" stance as
        // DuplicateCueConsolidator's own Conflict case. Needs a human.
        Conflict,
        // No healthy sibling found anywhere in this catalog. The
        // track is gone from this catalog entirely (covers both a lone
        // broken row with no match, and a group where every copy is
        // broken).
        Missing,
    };

    Kind kind = Kind::Missing;
    std::vector<Track> brokenGroup;      // every broken-file row folded into this issue
    std::optional<Track> survivor;       // set for Repairable/Conflict
    std::vector<CuePoint> survivorCues;  // set only when the survivor needs a cue write
};

// Pure classification: no filesystem or database access. Callers split a
// single catalog's own track list into healthyTracks (backing file
// confirmed present) and brokenTracks (confirmed missing) themselves.
// Domain code in this project never touches the filesystem directly, see
// gui/library_consistency_controller.cpp for that split. Call this once
// per catalog (rekordbox tracks against rekordbox tracks, etc.). Never
// pass tracks from two different catalogs together, since a healthy
// rekordbox copy says nothing about whether a broken Engine row is safe
// to repair.
class LibraryConsistencyChecker
{
public:
    static std::vector<LibraryConsistencyIssue> check(const std::vector<Track> &healthyTracks,
                                                        const std::vector<Track> &brokenTracks);
};

}  // namespace seabass::domain
