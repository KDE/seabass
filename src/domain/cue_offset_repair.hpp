// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain/mp3_lead_in.hpp"
#include "domain/track.hpp"

namespace seabass::domain
{

// Cues a few milliseconds off (#121): an MP3's cue that rekordbox and
// Engine both hold, a few tens of milliseconds from where the file's sound
// puts it on one of the two sides.
//
// rekordbox and Engine count an MP3's first sample differently, and the
// lead-in model (mp3_lead_in.hpp) says by how much for each file: the
// same sound sits `shift` ms later on rekordbox's timeline than on
// Engine's. Two things leave a pair of cues at another difference:
//
//   * Seabass builds before the model copied cues between the two
//     unmoved, so both sides hold the same number (a difference of 0).
//     The stick's log (seabass.log) names the direction of every such
//     copy: the side copied TO is the one that is off.
//   * A Denon player loading an import-marked row for the first time
//     moves its hot cues and loops by an amount of its own and takes the
//     mark off (5757c752, 6fbb52ac). Measured on a Prime 4 (Engine OS
//     5.3.0, 2026-11-23) with seven click tracks: 2292 samples when the
//     file has a valid encoder tag, 229 when it has none, nothing on a
//     WAV. Engine's side is the one that is off.
//
// The difference is not a spread: it is one of these exact values, so
// "a few milliseconds off" is a pair at exactly 0, or exactly at the
// player's shift, where the model says another value. Anything else is
// either right or a different cue, and Sync Cue Points' own business.
//
// Unsure means the DJ chooses: a pair at 0 that no log line explains is
// listed with both positions and never moved without the DJ's pick.

// A difference within this of the expected one is that one: rekordbox
// keeps whole milliseconds and Engine rounds to a sample.
constexpr double CueOffsetSameWithinMs = 1.5;
// The player's shift is a whole number of samples applied to a number
// rekordbox's import wrote, so it is recognised to the sample.
constexpr double PlayerShiftWithinSamples = 1.0;

// The samples a Denon player moves the hot cues and loops of an
// import-marked row by on its first load, for a file of this lead-in;
// nothing for a kind of file not measured. 2292 for a valid encoder tag
// (FFmpeg's "Lavc"/"Lavf", or LAME's with a correct CRC), 229 with none
// (no Info frame, an Info frame without an encoder string, a LAME tag
// whose CRC is wrong). VBRI, "L3.99", other encoder strings and a tag
// frame a decoder would not read were not measured.
std::optional<int> playerFirstLoadShiftSamples(const Mp3LeadIn &leadIn);

// One cue copy Seabass made between two catalogs, as its "sync: copied
// cues" line in the stick's log says it.
struct LoggedCueCopy
{
    std::string when;  // the line's timestamp, "2026-09-30T20:00:00"
    std::string sourceFormat;
    std::string sourceId;
    std::string sourceTitle;
    std::string targetFormat;
    std::string targetId;        // empty for a OneLibrary target, which is named by its file
    std::string targetFilename;  // a OneLibrary target's
};

// The copy a log line records, or nothing for any other line.
std::optional<LoggedCueCopy> parseLoggedCueCopy(std::string_view line);

enum class CueOffsetCause {
    // Both sides hold the same number where the model says `shift` apart.
    CopiedUnmoved,
    // Engine's sits the player's first-load shift before rekordbox's, on
    // a row whose import mark is gone.
    MovedByPlayer,
};

enum class CueOffsetSide { Engine, Rekordbox, Unknown };

// One cue both sides hold, off on one of them.
struct CueOffset
{
    size_t rekordboxIndex = 0;  // in CueOffsetFinding::rekordbox.cues
    size_t engineIndex = 0;     // in CueOffsetFinding::engine.cues
    CueOffsetCause cause = CueOffsetCause::CopiedUnmoved;
    // What this cue's evidence says is off: Engine for the player's shift
    // and for a logged copy onto Engine, rekordbox for one onto rekordbox,
    // Unknown when nothing says.
    CueOffsetSide side = CueOffsetSide::Unknown;
};

// One track whose cues are a few milliseconds off.
struct CueOffsetFinding
{
    // The rekordbox side ("rekordbox" or "onelibrary") and the Engine row,
    // as the catalogs were read: every position on rekordbox's timeline.
    Track rekordbox;
    Track engine;
    // rekordbox minus Engine for this file's sound (the lead-in model).
    double shiftMs = 0.0;
    int sampleRate = 0;
    std::optional<int> playerShiftSamples;
    std::vector<CueOffset> cues;
    // A summary: the side every cue's evidence names, or Unknown when they
    // do not all name the same one (or any cue names none). Each cue keeps
    // its own side; a repair moves the known ones where they say, and only
    // the unknown ones wait for the DJ's pick.
    CueOffsetSide sideToMove = CueOffsetSide::Unknown;
    // The logged copy that named a side, when one did.
    std::optional<LoggedCueCopy> loggedCopy;

    // The side the cue at `index` is moved on: its own when known, else
    // `pick` (nothing picked: Unknown, not moved).
    CueOffsetSide sideOf(size_t index, std::optional<CueOffsetSide> pick = std::nullopt) const;
    // Whether any cue is moved on `side`, the unknown ones on `pick`.
    bool moves(CueOffsetSide side, std::optional<CueOffsetSide> pick = std::nullopt) const;
    bool hasKnown() const;
    bool hasUnknown() const;
    // `side`'s whole cue set (Engine's or rekordbox's, on rekordbox's
    // timeline) with the cues moved on that side put where the other
    // side's are: what a repair of that side writes. The unknown cues move
    // only when `pick` names `side`.
    std::vector<CuePoint> cuesMoved(CueOffsetSide side, std::optional<CueOffsetSide> pick = std::nullopt) const;
    // Engine's position of the cue at `index` as the row stores it, on
    // Engine's own timeline.
    double engineStoredMs(size_t index) const;
    // One sentence for the DJ: what is off, by how much, and what says so.
    std::string reason() const;
};

// The cues of one matched pair that are a few milliseconds off, or
// nothing when none is (or the pair cannot be judged: not an MP3, its
// lead-in unread, the row still import-marked, or the Engine reader not
// on the model's timeline). `log` is the stick's cue copies, oldest
// first; the last one naming this pair decides a pair at 0.
std::optional<CueOffsetFinding> findCueOffsets(const Track &rekordboxSide, const Track &engine,
                                               const std::optional<Mp3LeadIn> &leadIn,
                                               const std::vector<LoggedCueCopy> &log);

}  // namespace seabass::domain
