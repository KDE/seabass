// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <chrono>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// Two tracks from different catalogs believed to be the same underlying
// song. Deliberately generic (trackA/trackB, not named after specific
// formats) -- this is reused for every pair of catalogs a stick might
// have (rekordbox<->Engine, rekordbox<->OneLibrary, Engine<->OneLibrary),
// see application::SyncLibraries's own doc comment. Each Track already
// carries its own catalog in Track::format, so callers needing to know
// which is which read trackA.format/trackB.format rather than this
// struct assuming fixed roles.
struct SyncMatch
{
    Track trackA;
    Track trackB;
};

// What to do about one SyncMatch's cues, decided without touching
// anything -- applying a plan is a separate, infrastructure-backed step.
struct SyncPlan
{
    enum class Kind {
        // Only one side has cues: propagate them to the other side.
        AOnly,
        BOnly,
        // Both sides have cues, and they differ: resolved by direction
        // (see below) using last-write-wins on the underlying file's
        // mtime, but the resolution is a heuristic, not a certainty --
        // reported clearly as a conflict either way.
        Conflict,
        // Both sides already have the same cues.
        AlreadyConsistent,
        // Neither side has any cues.
        NoCues,
    };
    enum class Direction { None, ToA, ToB };

    Kind kind = Kind::NoCues;
    SyncMatch match;
    Direction direction = Direction::None;
    std::vector<CuePoint> cuesToApply;  // the source side's cues, when direction != None
};

// Matches tracks across two catalog scans. See domain::matchTracks() for
// the actual signal priority (exact file path first -- the same physical
// file on the same stick, format-agnostic and by far the most reliable
// signal available -- falling back to title+artist/filename + duration
// tolerance only when a file path is missing on one side, e.g. a broken
// row).
class TrackMatcher
{
public:
    static std::vector<SyncMatch> match(const std::vector<Track> &tracksA, const std::vector<Track> &tracksB);
};

// Decides what should happen to one matched pair's cues. mtimeA/mtimeB
// are the last-modified times of each side's underlying data file, used
// only to break ties on a genuine conflict (both sides have different
// cues) -- documented as a heuristic in the plan, not a true edit
// timestamp.
class SyncPlanner
{
public:
    static SyncPlan plan(const SyncMatch &match, std::chrono::system_clock::time_point mtimeA,
                          std::chrono::system_clock::time_point mtimeB);
};

}  // namespace seabass::domain
