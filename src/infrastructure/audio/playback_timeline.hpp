// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>

#include "domain/mp3_lead_in.hpp"

namespace seabass::infrastructure::audio
{

// The timeline Seabass plays an MP3 on is Engine's, not rekordbox's (#50):
// Qt Multimedia decodes through FFmpeg at its defaults, which trims the
// encoder delay a LAME or Lavc tag declares and never plays the tag
// frame, the same as Engine DJ. qt_decoder_timeline_test measures it on
// the click files. Everything the player is handed is on another
// timeline: cues are kept on rekordbox's (engine_cue_timeline.hpp), and a
// beat grid is on the timeline of the library it was read from. This is
// what to add to each before the player, the ring or the play key uses
// it, so a hot cue jumps to the sound it marks and the ring pulses on
// the beat and not up to 52 ms early.
//
// The player may take every cue list it is handed to be on rekordbox's
// timeline, whichever library it came from, because the catalog's Tracks
// and Cues stages both resolve an Engine MP3 row's lead-in, falling back
// to reading the file itself when the stick's metadata cache has none
// (LibraryCatalogCache): an Engine row's cues are shifted whenever the
// player could read the same file to shift them back. The one remaining
// way for the two to disagree, the Preferences toggle of the correction
// changing after the rows were read, is closed by the catalog cache
// treating an Engine entry read under the other setting as stale.

// Which library's analysis the beat grid came from.
enum class GridTimeline { Rekordbox, Engine };

struct PlaybackShift
{
    // Added to every cue position (and loop end).
    double cuesMs = 0.0;
    // Added to every beat time.
    double beatsMs = 0.0;
};

// From a lead-in already read: 0/0 for a file without one, or while the
// correction is off (MatchingPolicy::correctMp3CueTiming), when the
// positions are left exactly as the libraries hold them.
PlaybackShift playbackShiftFor(const std::optional<domain::Mp3LeadIn> &leadIn, bool correct, GridTimeline grid);

// Reads the lead-in of `filePath` (nothing is read for a file that is not
// an MP3 by extension) and applies the policy.
PlaybackShift playbackShiftFor(const std::string &filePath, GridTimeline grid);

// What to add to a cue position (held on rekordbox's timeline) to put it
// on the timeline of `grid`'s beat grid for this file, where a view that
// draws the grid draws the cue: the player's shifts for both, one taken
// off the other. For an Engine MP3 Engine's lead-in difference, 0 for a
// rekordbox grid, which is on the cues' timeline already. Taking it off
// a position picked on such a view puts that position back on the cues'
// timeline, which is what a cue writer takes (#123).
double cueToGridShiftMs(const std::string &filePath, GridTimeline grid);

// rekordbox's position minus the decoder's for the same instant in this
// file, in ms: what to add to a time measured on the decoder's timeline
// (Qt Multimedia's, which is Engine's) to put it on the one cues are held
// on. What turns a shift between two copies measured by decoding them
// into a shift between their cues (domain/duplicate_cue_shift.hpp).
//
//   * 0 while the correction is off (MatchingPolicy::correctMp3CueTiming):
//     the same as playbackShiftFor(), which then plays cues exactly
//     where the libraries hold them, so the decoder's timeline and the
//     cues' are taken to be one.
//   * 0 for a file that is not an MP3. ASSUMED, NOT MEASURED: that
//     rekordbox and the decoder agree on where an M4A, FLAC, WAV or AIFF
//     file starts has not been checked the way #50 checked MP3s.
//   * engineToRekordboxOffsetMs() of the lead-in for an MP3.
//   * nullopt for an MP3 whose lead-in cannot be read. Unlike
//     playbackShiftFor(), which plays such a file at 0 because the
//     correction is never worse than none, a shift between two copies
//     that is up to 52 ms out would be written into cues, and a value
//     that cannot be known is not guessed.
// The track import and Analyse again move Seabass's own analysis onto
// rekordbox's timeline by this: a change to what it gives for a file
// raises AnalysisInputRevision (qt_multimedia_pcm_source.hpp).
std::optional<double> cueTimelineOffsetMs(const std::string &filePath);

}  // namespace seabass::infrastructure::audio
