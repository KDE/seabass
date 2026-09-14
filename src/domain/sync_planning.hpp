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
        // Both sides have cues, and they differ. Hot cues go to the side
        // edited more recently -- each track's own edit time when both
        // catalogs record one, the catalog file's mtime otherwise. Memory
        // cues are never resolved by overwriting Engine's one: when only
        // memory cues differ, the side that can hold them all receives the
        // union; when hot cues send the plan onto Engine, Engine keeps its
        // own memory cue. Still a heuristic rather than a certainty, and
        // reported clearly as a conflict either way.
        //
        // An Engine track whose memory cues are all among the other side's
        // is not a conflict: Engine holds one memory cue, and one of
        // rekordbox's three is agreement, not a difference.
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

    // Both sides have hot cues and they differ: a choice for the user, not
    // for a clock. Engine's Track.lastEditTime is moved by the database's
    // own triggers on ANY track edit -- a rating, a BPM change -- while a
    // rekordbox track's ANLZ mtime moves only when its cues change, so
    // "newer" cannot say whose hot cues are the ones the DJ meant. When
    // set, direction and cuesToApply hold only the suggestion (the newer
    // side); a caller that cannot ask must not apply it. The two lists
    // are what each choice would write, memory cue rules included.
    bool hotCuesNeedChoice = false;
    std::vector<CuePoint> cuesIfAWins;  // written onto B
    std::vector<CuePoint> cuesIfBWins;  // written onto A
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

// What putting `written` on a track that currently has `current` does to
// its cues, counted the way a DJ reads it. A cue already on the track at
// the same place -- and, for a hot cue, on the same pad -- is kept; one
// that is not is gained; one of the track's own that `written` leaves out
// is dropped. Positions compare with the tolerance the planner itself
// uses, so a cue that drifted by a cross-format rounding is kept here too.
//
// Exists for the Sync Cue Points page. "Copy 4 hot cues" onto a track that
// already has one of those four reads as four new cues when it is three,
// and a pad that moved reads as nothing lost when one was.
struct CueChange
{
    int gainedHot = 0;
    int keptHot = 0;
    int droppedHot = 0;
    int gainedMemory = 0;
    int keptMemory = 0;
    int droppedMemory = 0;
};

CueChange describeCueChange(const std::vector<CuePoint> &current, const std::vector<CuePoint> &written);

}  // namespace seabass::domain
