// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>

namespace seabass::domain
{

// How far apart the same music sits in two files.
//
// Two copies of one song are often not sample-aligned: one rip keeps a
// little more lead-in than the other, an encoder pads, a re-export
// trims. Measured on a real stick, of 290 pairs that were not
// byte-identical 283 lined up cleanly, and only 156 of those with no
// shift at all: 78 were 10 to 30 ms apart, 24 were 30 to 60 ms, 15 were
// 60 to 200 ms and 9 were 0.2 to 1.3 s. A cue copied from one copy onto
// the other with its position unchanged lands that far from the sound it
// marked. Neither the difference in length nor where the silence ends
// predicts the shift (one pair was 4.6 s longer and 160 ms shifted,
// another 0.08 s longer and 194 ms shifted; comparing silence onsets was
// wrong by up to 152 ms), so the only honest answer comes from decoding
// both files in full and comparing the audio itself.
//
// A port in the domain beside AudioContentProbe, for the same reason:
// the step that fills DuplicateGroup's shifts is domain and includes
// nothing from the layers above. The implementations live in
// infrastructure/audio.
class AudioAlignmentProbe
{
public:
    virtual ~AudioAlignmentProbe() = default;

    // Where a sound sits in `b` minus where the same sound sits in `a`,
    // in ms, on the decoder's own timeline: positive when the music in
    // `b` starts later.
    //
    // nullopt is "no opinion", and covers two different things on
    // purpose: nothing could be said (no decoder in this build, a file
    // that is unreadable or gone, a decode that timed out, a cancelled
    // scan) and the two files do not line up cleanly (different edits, a
    // section cut from one, a different recording). A caller must never
    // move a cue across a pair it got nullopt for: this feeds writes.
    virtual std::optional<double> shiftMs(const std::string &absolutePathA, const std::string &absolutePathB) = 0;

    // True only when the two files are known to hold the same bytes. Such
    // a pair lines up by definition on every timeline at once, so a caller
    // need not convert the shift between timelines either: the two files'
    // offsets are equal even where neither can be read. False is "not
    // known to be", never "known not to be".
    virtual bool sameContent(const std::string &, const std::string &) { return false; }
};

}  // namespace seabass::domain
