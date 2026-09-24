// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Three hot cues inside the first couple of seconds of a track. Nobody
// cues a track three times before the first bar is out; the write path
// fixed in #33 did, leaving them in the legacy PCOB list while the two
// real cues lived only in PCO2.
//
// The half that matters here is what this must NOT flag. Every cue it
// names is one a user will be offered the chance to delete, and a DJ's
// deliberate cue deleted on a guess is worse than an untidy one left
// alone. So the cases below spend most of their time on the shapes that
// look similar and are not the fault: a pair near the start, a loop, a
// third cue just outside the window.
//
// The last case runs the rule over the committed fixture, which carries
// the very track the issue was filed about.

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "domain/clustered_cue.hpp"
#include "domain/junk_cue.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"

namespace fs = std::filesystem;
using seabass::domain::ClusteredCueFinder;
using seabass::domain::CuePoint;
using seabass::domain::removableClusterCues;
using seabass::domain::Track;

namespace
{

CuePoint hot(int number, double positionMs)
{
    CuePoint cue;
    cue.kind = CuePoint::Kind::Hot;
    cue.hotCueNumber = number;
    cue.positionMs = positionMs;
    return cue;
}

CuePoint memory(double positionMs)
{
    CuePoint cue;
    cue.kind = CuePoint::Kind::Memory;
    cue.hotCueNumber = 0;
    cue.positionMs = positionMs;
    return cue;
}

Track trackWith(const std::string &title, std::vector<CuePoint> cues)
{
    Track track;
    track.sourceId = title;
    track.title = title;
    track.durationSeconds = 300.0;
    track.cues = std::move(cues);
    return track;
}

}  // namespace

int main()
{
    // The real one, from the stick the issue was filed about: three pads
    // inside 1.7 seconds, beside two that are plainly a person's work.
    {
        const Track track = trackWith("Too Little Too Late",
                                      {hot(1, 247.0), hot(2, 1188.0), hot(3, 1657.0), hot(4, 67751.0),
                                       hot(5, 135251.0)});
        const auto issues = ClusteredCueFinder::find({track});
        assert(issues.size() == 1);
        assert(issues[0].cluster.size() == 3);
        assert(issues[0].cluster[0].positionMs == 247.0);   // sorted, earliest first
        assert(issues[0].cluster[2].positionMs == 1657.0);
        // The two real cues are not in it and are never offered.
        for (const auto &cue : issues[0].cluster) {
            assert(cue.positionMs < 2000.0);
        }

        // The 247 ms one is already a stray cue by the first-second
        // rule, and is already offered there. Offering it here too would
        // let the same removal be staged from two places.
        const auto removable = removableClusterCues(issues[0]);
        assert(removable.size() == 2);
        assert(removable[0].positionMs == 1188.0);
        assert(removable[1].positionMs == 1657.0);
        std::cout << "case 1 (the cluster from the real stick is found, minus the cue already offered) OK\n";
    }

    // A pair is not a cluster. An intro marker and the first beat is an
    // ordinary thing to set, and the fixture measurement says two near
    // the start is a shape real libraries have.
    {
        assert(ClusteredCueFinder::find({trackWith("A pair", {hot(1, 300.0), hot(2, 1500.0)})}).empty());
        assert(ClusteredCueFinder::find({trackWith("One", {hot(1, 900.0)})}).empty());
        assert(ClusteredCueFinder::find({trackWith("None", {})}).empty());
        std::cout << "case 2 (one or two hot cues near the start is not a cluster) OK\n";
    }

    // Three cues that reach past the window are three cues somebody set.
    // The third at 2.5 s is outside it, so the other two are a pair.
    {
        assert(ClusteredCueFinder::find({trackWith("Spread out",
                                                   {hot(1, 250.0), hot(2, 1200.0), hot(3, 2500.0)})})
                   .empty());
        // And exactly on the boundary is outside it, not inside.
        assert(ClusteredCueFinder::find({trackWith("On the edge",
                                                   {hot(1, 250.0), hot(2, 1200.0), hot(3, 2000.0)})})
                   .empty());
        assert(ClusteredCueFinder::find({trackWith("Just inside",
                                                   {hot(1, 250.0), hot(2, 1200.0), hot(3, 1999.0)})})
                   .size() == 1);
        std::cout << "case 3 (the window is the first two seconds, and its edge is outside it) OK\n";
    }

    // Loops and memory cues are not pads. A loop carries an end, which a
    // stray write never does; a memory cue near the start is ordinary
    // (Engine's own analysis puts its main cue a few hundred ms in).
    {
        CuePoint loop = hot(2, 1200.0);
        loop.isLoop = true;
        loop.loopEndMs = 5000.0;
        assert(ClusteredCueFinder::find({trackWith("With a loop", {hot(1, 250.0), loop, hot(3, 1700.0)})}).empty());
        assert(ClusteredCueFinder::find(
                   {trackWith("Memory cues", {memory(250.0), memory(1200.0), memory(1700.0), hot(1, 400.0)})})
                   .empty());
        std::cout << "case 4 (a loop is not a pad, and memory cues are another check's business) OK\n";
    }

    // A negative position is a "no cue set" sentinel read as a position.
    // It points nowhere, so it is not evidence that somebody's pads were
    // written over, and three of them are not a cluster.
    {
        assert(ClusteredCueFinder::find(
                   {trackWith("Sentinels", {hot(1, -0.0226757), hot(2, -0.0226757), hot(3, -0.0226757)})})
                   .empty());
        // With two real early ones beside it, the sentinel still does not
        // count, so this is a pair and stays alone.
        assert(ClusteredCueFinder::find({trackWith("Sentinel and a pair",
                                                   {hot(1, -0.0226757), hot(2, 1200.0), hot(3, 1700.0)})})
                   .empty());
        std::cout << "case 5 (a negative position is a sentinel, not a pad) OK\n";
    }

    // The committed fixture, which is where the window came from. The
    // rule was chosen because widening it from two seconds to eight
    // finds nothing more, and because three-in-a-window happens once in
    // 2725 tracks while two happens three times.
    //
    // Asserted as "exactly one, and it is that one" rather than as a
    // count alone: a rule that flagged half the library would also
    // satisfy a >= 1 assertion.
    {
        const fs::path fixture = seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "anonymized_library";

        seabass::infrastructure::rekordbox::KaitaiRekordboxReader rekordbox(seabass::pathToUtf8(fixture / "rekordbox"));
        const auto rekordboxTracks = rekordbox.readAll();
        assert(rekordboxTracks.size() > 1000 && "the fixture must still be the whole library");
        const auto rekordboxIssues = ClusteredCueFinder::find(rekordboxTracks);
        for (const auto &issue : rekordboxIssues) {
            std::cout << "   rekordbox: " << issue.track.title << " (" << issue.track.artist << ")";
            for (const auto &cue : issue.cluster) {
                std::cout << " " << cue.positionMs << "ms";
            }
            std::cout << "\n";
        }
        assert(rekordboxIssues.size() == 1 && "one track in the fixture has three hot cues in its first two seconds");
        assert(rekordboxIssues[0].cluster.size() == 3);
        assert(rekordboxIssues[0].cluster[0].positionMs == 247.0);
        assert(rekordboxIssues[0].cluster[1].positionMs == 1188.0);
        assert(rekordboxIssues[0].cluster[2].positionMs == 1657.0);
        assert(removableClusterCues(rekordboxIssues[0]).size() == 2);

        seabass::infrastructure::engine::LibdjinteropEngineReader engine(seabass::pathToUtf8(fixture / "engine"));
        const auto engineTracks = engine.readAll();
        assert(engineTracks.size() > 1000);
        // The Engine side of the same library has no cluster at all: its
        // one near-the-start pair is a pair. That is the half of the
        // measurement that says this rule is not simply flagging
        // everything with cues on it.
        assert(ClusteredCueFinder::find(engineTracks).empty());

        std::cout << "case 6 (over the committed fixture: one clustered track in rekordbox, none in Engine) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
