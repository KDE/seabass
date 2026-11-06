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

}  // namespace seabass::infrastructure::audio
