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

    // What the probe found for one pair, three ways, because they are
    // kept apart further on: a cache remembers the first two and never
    // the third.
    struct Answer
    {
        enum class Kind {
            // The same music, `shiftMs` apart.
            LinedUp,
            // Both files were decoded whole and do not line up cleanly
            // (different edits, a section cut from one, another
            // recording, music too repetitive to tell one bar from the
            // next). A property of the two files: asking again gives the
            // same answer.
            NotLinedUp,
            // Nothing could be said this time: no decoder, a file that is
            // unreadable or gone, a decode that timed out, a cancelled
            // scan. `unreadableA`/`unreadableB` say when it was a file.
            NoOpinion,
        };
        Kind kind = Kind::NoOpinion;
        double shiftMs = 0.0;  // LinedUp only
        bool unreadableA = false;
        bool unreadableB = false;

        static Answer linedUp(double ms) { return {Kind::LinedUp, ms, false, false}; }
        static Answer notLinedUp() { return {Kind::NotLinedUp, 0.0, false, false}; }
        static Answer noOpinion(bool unreadableA = false, bool unreadableB = false)
        {
            return {Kind::NoOpinion, 0.0, unreadableA, unreadableB};
        }
    };

    // Where a sound sits in `b` minus where the same sound sits in `a`,
    // in ms, on the decoder's own timeline: positive when the music in
    // `b` starts later. A caller must never move a cue across a pair
    // whose answer is not LinedUp: this feeds writes.
    virtual Answer align(const std::string &absolutePathA, const std::string &absolutePathB) = 0;

    // align() as a shift or nothing, for a caller to whom the two kinds
    // of nothing are the same.
    std::optional<double> shiftMs(const std::string &absolutePathA, const std::string &absolutePathB)
    {
        const Answer answer = align(absolutePathA, absolutePathB);
        return answer.kind == Answer::Kind::LinedUp ? std::optional<double>(answer.shiftMs) : std::nullopt;
    }

    // True when align() would answer this pair from what it already
    // holds, without decoding anything. For a progress bar to spread its
    // stretch over the pairs that cost something.
    virtual bool answersAtOnce(const std::string &, const std::string &) { return false; }

    // True only when the two files are known to hold the same bytes. Such
    // a pair lines up by definition on every timeline at once, so a caller
    // need not convert the shift between timelines either: the two files'
    // offsets are equal even where neither can be read. False is "not
    // known to be", never "known not to be".
    virtual bool sameContent(const std::string &, const std::string &) { return false; }
};

}  // namespace seabass::domain
