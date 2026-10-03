// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "domain/cue_tolerance.hpp"
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
//
// The rule (2026-10-04): everything the planner cannot know for sure is a
// choice for the DJ, and every choice says why (reason, reasonText). No
// clock decides anything: an XDJ-RX2 was measured stamping 2017, a Prime 4
// two hours off, and FAT times shift with the time zone. Measurements
// remove reasons over time; until something is measured, the DJ is asked.
struct SyncPlan
{
    enum class Kind {
        // Only one side has cues: propagate them to the other side.
        AOnly,
        BOnly,
        // Both sides have cues, and they differ. Either one side is only
        // missing something the other has, and the plan copies it across
        // (direction set, nothing of the target's is lost: memory cues go
        // as a union, and a side with no hot cues of its own receives the
        // other's), or the planner cannot tell which side is right, and
        // needsChoice is set with a reason.
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

    // Why a plan needs the DJ's choice. Tests assert this; the page and the
    // CLI show reasonText.
    enum class Reason {
        None,
        // A pad holds different cues, or is empty on one side.
        PadsDiffer,
        // Both sides have the pad, more than the tolerance apart but less
        // than a second: the case a tight tolerance exists to show.
        SamePadApart,
        // Same pad, a loop on one side and a plain cue on the other.
        LoopVsCue,
        // The same loop on both sides, starting at the same place, ending
        // more than the tolerance apart: its length was changed on one
        // side. A pad's loop, or a memory loop.
        LoopEndsDiffer,
        // An Engine pad sits at one of the other side's memory cues, on a
        // pad Engine DJ's import would not have given that memory cue.
        EngineMemoryOrHotCue,
        // Both sides have something the other lacks, and only a clock
        // could say which edit came last.
        BothChanged,
        // The two sides do not agree on a tempo (or have none), so half a
        // beat could not be used, and the fallback decided something.
        TempoUnsure,
        // Two catalogs propose different cues for a third
        // (domain::CrossSourceSyncConflict, never set by SyncPlanner).
        SourcesDisagree,
    };

    Kind kind = Kind::NoCues;
    SyncMatch match;
    Direction direction = Direction::None;
    std::vector<CuePoint> cuesToApply;  // the source side's cues, when direction != None

    // A choice for the DJ. direction is None and cuesToApply empty: a
    // caller that cannot ask writes nothing. The two lists are what each
    // choice would write, memory cue rules included.
    bool needsChoice = false;
    std::vector<CuePoint> cuesIfAWins;  // written onto B
    std::vector<CuePoint> cuesIfBWins;  // written onto A
    Reason reason = Reason::None;
    // One line for a DJ, set with reason: "Pad 3: rekordbox 1:07.751,
    // Engine 0:30.251". No dashes (the UI's rule), positions as m:ss.mmm.
    std::string reasonText;

    // The position tolerance this pair was compared with (half a beat, or
    // the fallback), for anything counting the plan's cues afterwards.
    double positionToleranceMs = CueFallbackToleranceMs;

    // Memory cues and loops of the non-Engine side that no Engine pad was
    // free for (domain::translateCuesForEngine). Never written onto
    // Engine, never counted as missing there. Set whenever one side is
    // Engine, whatever the direction, so the page and the CLI can say so
    // (describeCuesLeftOut).
    std::vector<CuePoint> cuesLeftOut;
};

// "2 cues stay off Engine: its eight pads are full (1:02.000, 3:10.500)",
// or empty for none.
std::string describeCuesLeftOut(const std::vector<CuePoint> &cuesLeftOut);

// The same cues the way the planner compares them: the same pads, each a
// cue or a loop alike, the same memory cues and loops, positions and loop
// ends within `toleranceMs` (domain::sameCuePlace).
bool sameCuesForSync(const std::vector<CuePoint> &a, const std::vector<CuePoint> &b, double toleranceMs);

// A cue position the way reasons spell it: m:ss.mmm.
std::string formatCuePosition(double positionMs);

// "rekordbox", "Engine", "OneLibrary": a Track::format the way reasons
// name it.
std::string catalogDisplayName(const std::string &format);

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
// are the last-modified times of each side's underlying data file. They
// decide nothing (see SyncPlan): the callers still pass them, and a page
// may show them, but no plan's direction depends on a clock.
//
// Positions are compared within half a beat of the tempo both sides agree
// on (domain/cue_tolerance.hpp), else within the fallback, and then any
// pair that half a beat might have matched is a choice
// (SyncPlan::Reason::TempoUnsure).
//
// With an Engine side, the other side is compared with Engine the way
// Engine DJ's own rekordbox import maps cues (domain/engine_cue_translation.hpp):
// memory cues are hot cues on free pads there, memory loops saved loops,
// and an Engine hot cue at a memory cue's position is that memory cue.
//
// Two kinds of cue never make a pair inconsistent. A cue the preference
// "Ignore cues at 0:00" ignores (domain::isJunkCue) is left out of every
// comparison and every write, so it neither travels nor survives the
// other side's cues being written over its track. And a memory cue no
// Engine pad is free for, or that Engine cannot hold as its main cue
// (domain::engineCanHoldMemoryCue), is never something Engine lacks.
// Either way a sync comes back clean after one save; before this, a
// stray memory cue at 0:00 was offered to Engine again on every sync,
// the write having stored nothing.
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
// is dropped. Positions compare within `toleranceMs`: pass the plan's
// positionToleranceMs, so a cue that drifted by a cross-format rounding
// is kept here too.
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

CueChange describeCueChange(const std::vector<CuePoint> &current, const std::vector<CuePoint> &written,
                            double toleranceMs = CueFallbackToleranceMs);

}  // namespace seabass::domain
