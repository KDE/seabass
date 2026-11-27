// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// domain::measureCuesAgainstBeats, offBeatWithin and cuesAlignedToBeats: a
// cue is measured against the nearest beat, a millisecond or less is on
// the beat, the tolerance is applied afterwards, a cue further off than
// the tolerance stays where it was put, a loop keeps its length unless its
// end is off a beat too, and cues all off by one amount are said to be.

#include <cassert>
#include <cmath>
#include <iostream>

#include "domain/cue_beat_alignment.hpp"

using namespace seabass::domain;

namespace
{

CuePoint cue(double ms, double loopEndMs = 0.0)
{
    CuePoint c;
    c.kind = CuePoint::Kind::Hot;
    c.positionMs = ms;
    c.isLoop = loopEndMs > 0.0;
    c.loopEndMs = loopEndMs;
    return c;
}

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-9;
}

}  // namespace

int main()
{
    // 120 BPM from 100 ms: a beat every 500 ms.
    std::vector<double> beats;
    for (int i = 0; i < 64; ++i) {
        beats.push_back(100.0 + 500.0 * i);
    }

    // 1. On the beat, a hair off (how it is stored), 8 ms late, 12 ms
    //    early, half a beat off, before the first beat and after the last.
    {
        const std::vector<CuePoint> cues = {cue(1100.0),  cue(1600.7), cue(2108.0), cue(2588.0),
                                            cue(3350.0),  cue(40.0),   cue(40000.0)};
        const CueBeatAlignment a = measureCuesAgainstBeats(cues, beats);
        assert(a.points.size() == 7);
        assert(near(a.points[0].offMs, 0.0) && near(a.points[0].beatMs, 1100.0));
        assert(near(a.points[1].offMs, 0.7));
        assert(near(a.points[2].offMs, 8.0) && near(a.points[2].beatMs, 2100.0));
        assert(near(a.points[3].offMs, -12.0) && near(a.points[3].beatMs, 2600.0) && "early: negative");
        assert(near(std::abs(a.points[4].offMs), 250.0));
        assert(near(a.points[5].offMs, -60.0) && near(a.points[5].beatMs, 100.0) && "against the first beat");
        assert(near(a.points[6].beatMs, 100.0 + 500.0 * 63) && "against the last beat");
        assert(!a.sameOffset);

        // The tolerance decides what is listed, with nothing measured again.
        assert(offBeatWithin(a, 5.0).empty() && "0.7 ms is how it was stored, 8 ms is past 5");
        const auto ten = offBeatWithin(a, 10.0);
        assert(ten.size() == 1 && ten[0].cueIndex == 2);
        const auto twenty = offBeatWithin(a, 20.0);
        assert(twenty.size() == 2 && twenty[1].cueIndex == 3);
        assert(offBeatWithin(a, 100.0).size() == 3 && "the one before the first beat too, not the off-beat one");

        const std::vector<CuePoint> aligned = cuesAlignedToBeats(cues, a, 20.0);
        assert(near(aligned[0].positionMs, 1100.0) && near(aligned[1].positionMs, 1600.7) && "on the beat: untouched");
        assert(near(aligned[2].positionMs, 2100.0) && near(aligned[3].positionMs, 2600.0));
        assert(near(aligned[4].positionMs, 3350.0) && "put off the beat on purpose: stays");
        assert(near(aligned[5].positionMs, 40.0) && near(aligned[6].positionMs, 40000.0) && "past the tolerance: stay");
        std::cout << "case 1 (measured once, listed and aligned by the tolerance) OK\n";
    }

    // 2. Loops. One whose start is 6 ms late and whose end is on no beat
    //    (a three-quarter-beat loop): it keeps its length. One whose start
    //    and end are each a few ms off their own beats: each goes to its
    //    beat. One on the beat with only its end off.
    {
        const std::vector<CuePoint> cues = {cue(1106.0, 1481.0), cue(2104.0, 4097.0), cue(5100.0, 6107.0)};
        const CueBeatAlignment a = measureCuesAgainstBeats(cues, beats);
        assert(a.points.size() == 6 && a.points[1].loopEnd && a.points[1].cueIndex == 0);
        const std::vector<CuePoint> aligned = cuesAlignedToBeats(cues, a, 20.0);
        assert(near(aligned[0].positionMs, 1100.0) && near(aligned[0].loopEndMs, 1475.0) && "the length is kept");
        assert(near(aligned[1].positionMs, 2100.0) && near(aligned[1].loopEndMs, 4100.0) && "both onto their beats");
        assert(near(aligned[2].positionMs, 5100.0) && near(aligned[2].loopEndMs, 6100.0) && "only the end moves");
        assert(aligned[0].isLoop && aligned[1].isLoop);
        std::cout << "case 2 (loops: the length kept, or each end onto its own beat) OK\n";
    }

    // 2b. Short loops, which have no beat of their own to end on: an
    //     eighth of a beat starting on the beat is not listed at all (its
    //     end is no cue point off a beat), and one starting 6 ms late moves
    //     as a whole. Neither is ever closed up. A loop of a whole beat
    //     less a few ms does end on a beat of its own.
    {
        const std::vector<CuePoint> cues = {cue(1100.0, 1162.5), cue(2106.0, 2121.0), cue(3100.0, 3594.0)};
        const CueBeatAlignment a = measureCuesAgainstBeats(cues, beats);
        assert(a.points.size() == 4 && "only the third loop's end is measured");
        assert(a.points[3].loopEnd && a.points[3].cueIndex == 2 && near(a.points[3].offMs, -6.0));
        assert(offBeatWithin(a, 100.0).size() == 2 && "the late start and the near-whole-beat end");
        const std::vector<CuePoint> aligned = cuesAlignedToBeats(cues, a, 100.0);
        assert(near(aligned[0].positionMs, 1100.0) && near(aligned[0].loopEndMs, 1162.5) && "untouched");
        assert(near(aligned[1].positionMs, 2100.0) && near(aligned[1].loopEndMs, 2115.0) && "moved whole, 15 ms long still");
        assert(near(aligned[2].positionMs, 3100.0) && near(aligned[2].loopEndMs, 3600.0));
        for (const CuePoint &c : aligned) {
            assert(c.loopEndMs > c.positionMs && "no loop is closed up");
        }
        std::cout << "case 2b (short loops keep their length and are never closed up) OK\n";
    }

    // 3. Every cue 26 ms late, as against a grid that is itself shifted:
    //    said, so nobody moves them all off the music. One of them on the
    //    beat among others off by different amounts: not said.
    {
        const CueBeatAlignment shifted = measureCuesAgainstBeats({cue(1126.0), cue(4126.4), cue(9125.2)}, beats);
        assert(shifted.sameOffset);
        const CueBeatAlignment mixed = measureCuesAgainstBeats({cue(1126.0), cue(4108.0), cue(9100.0)}, beats);
        assert(!mixed.sameOffset);
        const CueBeatAlignment one = measureCuesAgainstBeats({cue(1126.0), cue(4100.0)}, beats);
        assert(!one.sameOffset && "one cue off is no pattern");
        std::cout << "case 3 (cues all off by one amount) OK\n";
    }

    // 4. No grid to speak of: nothing measured, nothing moved.
    {
        const std::vector<CuePoint> cues = {cue(1108.0)};
        const CueBeatAlignment none = measureCuesAgainstBeats(cues, {});
        assert(none.points.empty() && !none.sameOffset);
        assert(measureCuesAgainstBeats(cues, {100.0}).points.empty());
        assert(near(cuesAlignedToBeats(cues, none, 20.0)[0].positionMs, 1108.0));
        std::cout << "case 4 (a grid of fewer than two beats) OK\n";
    }
    return 0;
}
