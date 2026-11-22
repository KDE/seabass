// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "domain/audio_content_probe.hpp"
#include "domain/track.hpp"

namespace seabass::domain
{

// Two or more tracks in the same library scan that are believed to be
// copies of the same underlying song (same filename, matching duration).
struct DuplicateGroup
{
    std::vector<Track> tracks;

    // Per copy, parallel to `tracks`: where a sound sits in that copy
    // minus where it sits in the FIRST copy, in ms on the cue timeline
    // (rekordbox's, the one Track::cues are held on). nullopt when it
    // could not be measured or the two do not line up cleanly; the first
    // copy's own is 0. See duplicate_cue_shift.hpp, which fills it in
    // after grouping (DuplicateTrackFinder::find() leaves it empty).
    //
    // Empty means not measured, which the measuring step leaves only on
    // a group none of whose copies carries a cue, where nothing can move.
    // Read empty as "the copies line up": that is also what a group put
    // together by hand in a test means.
    std::vector<std::optional<double>> cueShiftMs;
};

// What to do about one DuplicateGroup's cues, decided without touching
// anything -- applying the plan is a separate, infrastructure-backed step.
struct ConsolidationPlan
{
    enum class Kind {
        // Exactly one copy has cues, the rest have none: propagate source's
        // cues onto every track in targets.
        Unambiguous,
        // More than one copy has cues, and they differ: a human has to
        // decide, we never guess.
        Conflict,
        // Every copy that has cues already has the same ones.
        AlreadyConsistent,
        // No copy in the group has any cues at all.
        NoCues,
        // Copies with cues, and some copy whose music could not be lined
        // up with the others' (DuplicateGroup::cueShiftMs): nothing can
        // be copied or compared across that pair without guessing where
        // its cues belong, so like Conflict a human decides.
        Unaligned,
    };

    Kind kind = Kind::NoCues;
    DuplicateGroup group;
    std::optional<Track> source;  // set only for Kind::Unambiguous
    std::vector<Track> targets;   // set only for Kind::Unambiguous
    // Parallel to targets: what to add to a source cue's position to put
    // it on that target's timeline (see cueShiftBetween()). Always known
    // here, because a group with an unknown one is Kind::Unaligned.
    std::vector<double> targetShiftMs;
};

// Groups tracks that look like duplicates of each other. Two tracks are
// duplicates when they agree on artist + title AND on length (within
// MatchingPolicy::exactMatchSeconds(), or -- inside the wider window
// that setting's neighbour defines -- on the length of their audio once
// leading and trailing silence is taken off). All three are required:
// filename is not a matching criterion (export-assigned number prefixes
// and copy suffixes differ between copies of one song, and truncation
// can make two different songs share a name), and a track whose length
// is unknown is never grouped -- artist + title alone cannot
// distinguish a radio edit from an extended mix, and this feeds a
// destructive caller.
// Format-agnostic -- has no notion of which catalog a Track came from,
// so callers can pass a single library's tracks (intra-library
// duplicates) or a concatenation of both rekordbox's and Engine's tracks
// (cross-library duplicates, used by the Clean Up feature) with no
// difference in behavior.
class DuplicateTrackFinder
{
public:
    // `probe`, when given, is what settles a pair whose lengths differ
    // by more than MatchingPolicy::exactMatchSeconds() but no more than
    // MatchingPolicy::compareAudioSeconds(): the two files are decoded
    // far enough to find where the music starts and stops, and the
    // lengths of the music -- not of the files -- are compared. Without
    // one (no decoder in this build, or the user set the wider window
    // to the same value as the exact one) only stored lengths are used
    // and a pair outside the exact window is simply not a duplicate.
    // A probe can only ever find MORE. The clustering runs in two
    // passes for exactly this reason: the first uses stored lengths
    // alone, the second only attaches tracks the first left on their
    // own, and never takes a cluster the first built apart. A single
    // pass that consulted the probe inline could make an already-found
    // pair disappear when the setting was switched on -- see the worked
    // example in find()'s own body.
    static std::vector<DuplicateGroup> find(const std::vector<Track> &tracks,
                                             AudioContentProbe *probe = nullptr);
};

// Decides, for a single DuplicateGroup, whether its cues can be
// consolidated unambiguously.
class DuplicateCueConsolidator
{
public:
    static ConsolidationPlan plan(const DuplicateGroup &group);
};

}  // namespace seabass::domain
