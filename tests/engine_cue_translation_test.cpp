// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// How a cue set from a format with memory cues becomes what Engine holds,
// and how Engine's cues read back in that format's terms. The mapping is
// the one Engine DJ's own rekordbox import applies.

#include <cassert>
#include <iostream>
#include <vector>

#include "domain/engine_cue_translation.hpp"

using namespace seabass::domain;

namespace
{

CuePoint hot(int pad, double ms, bool loop = false)
{
    CuePoint cue;
    cue.kind = CuePoint::Kind::Hot;
    cue.hotCueNumber = pad;
    cue.positionMs = ms;
    cue.isLoop = loop;
    cue.loopEndMs = loop ? ms + 4000.0 : 0.0;
    return cue;
}

CuePoint memory(double ms, bool loop = false)
{
    CuePoint cue;
    cue.kind = CuePoint::Kind::Memory;
    cue.positionMs = ms;
    cue.color = "#123456";
    cue.isLoop = loop;
    cue.loopEndMs = loop ? ms + 4000.0 : 0.0;
    return cue;
}

std::vector<int> padsOf(const std::vector<CuePoint> &cues)
{
    std::vector<int> pads;
    for (const auto &cue : cues) {
        if (cue.kind == CuePoint::Kind::Hot) {
            pads.push_back(cue.hotCueNumber);
        }
    }
    return pads;
}

}  // namespace

int main()
{
    // Hot cues keep their pads; memory cues take the free ones in time
    // order; the earliest memory cue is the main cue.
    {
        auto t = translateCuesForEngine({hot(1, 1000.0), hot(3, 3000.0), memory(90000.0), memory(60000.0)}, {});
        assert(t.leftOut.empty());
        assert((padsOf(t.cues) == std::vector<int>{1, 3, 2, 4}));
        assert(t.cues[2].positionMs == 60000.0 && t.cues[2].color == "#123456" && "colour travels onto the pad");
        assert(t.cues.back().kind == CuePoint::Kind::Memory && t.cues.back().positionMs == 60000.0);
        std::cout << "case 1 (memory cues become hot cues on the free pads) OK\n";
    }

    // A memory loop becomes a saved loop, after the cues, on a free pad,
    // and is never the main cue.
    {
        auto t = translateCuesForEngine({memory(10000.0, true), memory(20000.0)}, {});
        assert((padsOf(t.cues) == std::vector<int>{1, 2}));
        assert(t.cues[0].positionMs == 20000.0 && !t.cues[0].isLoop);
        assert(t.cues[1].positionMs == 10000.0 && t.cues[1].isLoop && t.cues[1].loopEndMs == 14000.0);
        assert(t.cues[2].kind == CuePoint::Kind::Memory && t.cues[2].positionMs == 20000.0);
        std::cout << "case 2 (a memory loop becomes a saved loop) OK\n";
    }

    // Pads full: the rest is left out, not silently dropped.
    {
        std::vector<CuePoint> cues;
        for (int pad = 1; pad <= 7; ++pad) {
            cues.push_back(hot(pad, pad * 1000.0));
        }
        cues.push_back(memory(50000.0));
        cues.push_back(memory(40000.0));
        auto t = translateCuesForEngine(cues, {});
        assert(padsOf(t.cues).size() == 8);
        assert(t.cues[7].hotCueNumber == 8 && t.cues[7].positionMs == 40000.0 && "the earliest gets the last pad");
        assert(t.leftOut.size() == 1 && t.leftOut[0].positionMs == 50000.0);
        std::cout << "case 3 (no pad free: left out and reported) OK\n";
    }

    // Engine already holds a translation on some pad: it stays there. Its
    // main cue stays too, even when it is not the earliest.
    {
        std::vector<CuePoint> existing = {hot(1, 1000.0), hot(5, 60000.0), memory(90000.0)};
        auto t = translateCuesForEngine({hot(1, 1000.0), memory(60000.0), memory(90000.0)}, existing);
        assert((padsOf(t.cues) == std::vector<int>{1, 5, 2}) && "the pad Engine had it on is kept");
        assert(t.cues.back().kind == CuePoint::Kind::Memory && t.cues.back().positionMs == 90000.0);
        std::cout << "case 4 (a sync does not shuffle pads or move the main cue) OK\n";
    }

    // A pad a hot cue now claims is no longer available to the memory cue
    // that was on it.
    {
        std::vector<CuePoint> existing = {hot(2, 60000.0)};
        auto t = translateCuesForEngine({hot(2, 2000.0), memory(60000.0)}, existing);
        assert((padsOf(t.cues) == std::vector<int>{2, 1}));
        std::cout << "case 5 (a hot cue outranks the memory cue on its pad) OK\n";
    }

    // Engine's cues in rekordbox's terms: a hot cue where rekordbox has a
    // memory cue is that memory cue, with rekordbox's own colour; the rest
    // are Engine's own; the main cue is a memory cue.
    {
        std::vector<CuePoint> engine = {hot(1, 1000.0), hot(2, 60000.0), hot(3, 10000.0, true), memory(30000.0)};
        engine[1].color = "";
        CuePoint rbMemory = memory(60000.0);
        rbMemory.color = "#abcdef";
        auto seen = cuesFromEngine(engine, {rbMemory, memory(10000.0, true)});
        assert(seen.hotCues.size() == 1 && seen.hotCues[0].hotCueNumber == 1);
        assert(seen.memoryCues.size() == 3);
        assert(seen.memoryCues[0].positionMs == 60000.0 && seen.memoryCues[0].color == "#abcdef"
               && seen.memoryCues[0].kind == CuePoint::Kind::Memory);
        assert(seen.memoryCues[1].isLoop && seen.memoryCues[1].positionMs == 10000.0);
        assert(seen.memoryCues[2].positionMs == 30000.0 && "the main cue");
        std::cout << "case 6 (Engine's cues read back in rekordbox's terms) OK\n";
    }

    // Within half a second is the same place, as everywhere else.
    {
        auto seen = cuesFromEngine({hot(1, 60400.0)}, {memory(60000.0)});
        assert(seen.hotCues.empty() && seen.memoryCues.size() == 1);
        std::cout << "case 7 (a rounding drift is still the same cue) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
