// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// domain::HiddenEngineCueFinder: an Engine track's hot cue or loop with
// no colour is one the player hides; other formats are not its business.

#include <cassert>
#include <iostream>

#include "domain/hidden_engine_cues.hpp"

using namespace seabass::domain;

namespace
{
CuePoint hot(int pad, double ms, const std::string &color, bool loop = false)
{
    CuePoint c;
    c.kind = CuePoint::Kind::Hot;
    c.hotCueNumber = pad;
    c.positionMs = ms;
    c.color = color;
    c.isLoop = loop;
    c.loopEndMs = loop ? ms + 2000.0 : 0.0;
    return c;
}
CuePoint memory(double ms)
{
    CuePoint c;
    c.kind = CuePoint::Kind::Memory;
    c.positionMs = ms;
    return c;
}
Track track(const std::string &format, const std::string &id, std::vector<CuePoint> cues)
{
    Track t;
    t.format = format;
    t.sourceId = id;
    t.title = "T" + id;
    t.cues = std::move(cues);
    return t;
}
}  // namespace

int main()
{
    // Two of four pads without a colour, one of them a loop: found, with
    // the counts a page needs. The cue point is not a pad and not counted.
    {
        auto found = HiddenEngineCueFinder::find({track("engine", "1", {hot(1, 1000, "#FF0000"), hot(3, 3000, ""),
                                                                     hot(4, 4000, "", true), memory(500)})});
        assert(found.size() == 1);
        assert(found[0].track.sourceId == "1");
        assert(found[0].hotCues == 1 && found[0].loops == 1 && found[0].hidden() == 2);
        std::cout << "case 1 (colourless pads on an Engine track are found and counted) OK\n";
    }

    // A track whose pads all have a colour is not listed, nor one with
    // no pads at all.
    {
        auto found = HiddenEngineCueFinder::find({track("engine", "2", {hot(1, 1000, "#00FF00"), memory(500)}),
                                                  track("engine", "3", {memory(500)}), track("engine", "4", {})});
        assert(found.empty());
        std::cout << "case 2 (coloured pads and empty tracks are not findings) OK\n";
    }

    // OneLibrary never stores a colour and rekordbox may not: not this
    // check's business, whatever their cues look like.
    {
        auto found = HiddenEngineCueFinder::find(
            {track("onelibrary", "5", {hot(1, 1000, "")}), track("rekordbox", "6", {hot(2, 2000, "")})});
        assert(found.empty());
        std::cout << "case 3 (only Engine tracks are looked at) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
