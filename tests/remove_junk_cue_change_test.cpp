// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What the accidental-cue removal actually strips.
//
// This class used to work the set out from the track alone, with
// isJunkCue(). That was right while one check fed the list. It stopped
// being right the moment a second one did (#41): a clustered hot cue
// sits past the first second, so it was listed, staged, and counted --
// and the rewrite left it on the stick. The same shape as the 12 ms bug
// this file's own comment records, and the reason the set is handed in
// now rather than re-derived.
//
// unitsWritten() is the observable here: it is the count the save
// reports, and it is computed from the same set apply() rewrites with,
// so a number that disagrees with what goes is the bug itself.

#include <QCoreApplication>

#include <cassert>
#include <iostream>
#include <vector>

#include "domain/junk_cue.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/remove_junk_cue_change.hpp"

using seabass::domain::CuePoint;
using seabass::domain::Track;
using seabass::gui::RemoveJunkCueChange;

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

Track trackWith(std::vector<CuePoint> cues)
{
    Track track;
    track.format = "rekordbox";
    track.sourceId = "1";
    track.title = "OLYMPIA";
    track.filePath = "/nowhere/olympia.mp3";
    track.cues = std::move(cues);
    return track;
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // The shape on the stick this was found against: hot cues at 505,
    // 999 and 1001 ms. The first two are inside the first second and
    // the old rule covers them; 1001 ms is a millisecond past it and
    // only the cluster check can see it.
    const Track track = trackWith({hot(1, 505.0), hot(2, 999.0), hot(3, 1001.0), hot(4, 90000.0)});
    assert(seabass::domain::isJunkCue(track.cues[0]));
    assert(seabass::domain::isJunkCue(track.cues[1]));
    assert(!seabass::domain::isJunkCue(track.cues[2]) && "the whole point: the old rule cannot see this one");

    // Handed nothing extra, it strips only what isJunkCue() covers.
    {
        RemoveJunkCueChange change("/nowhere", track);
        assert(change.unitsWritten() == 2);
    }

    // Handed the clustered cue the list showed, it strips that too.
    {
        RemoveJunkCueChange change("/nowhere", track, {hot(3, 1001.0)});
        assert(change.unitsWritten() == 3);
    }

    // Named cues that are not on the track change nothing, rather than
    // inflating the count the save reports.
    {
        RemoveJunkCueChange change("/nowhere", track, {hot(7, 4242.0)});
        assert(change.unitsWritten() == 2);
    }

    // Kind and pad number are part of identity, not just the position: a
    // hot cue and a memory cue can sit on the same millisecond, and only
    // one of them was in the list.
    {
        const Track both = trackWith({hot(1, 3000.0), memory(3000.0), hot(2, 90000.0)});
        RemoveJunkCueChange change("/nowhere", both, {hot(1, 3000.0)});
        assert(change.unitsWritten() == 1 && "the memory cue at the same position stays");
    }

    // A cue already covered by isJunkCue() and named again is removed
    // once, not counted twice.
    {
        RemoveJunkCueChange change("/nowhere", track, {hot(1, 505.0), hot(3, 1001.0)});
        assert(change.unitsWritten() == 3);
    }

    std::cout << "remove_junk_cue_change_test passed\n";
    return 0;
}
