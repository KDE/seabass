// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include "domain/local_restore.hpp"

using namespace seabass::domain;

Track makeTrack(std::string id, double duration, std::vector<CuePoint> cues)
{
    Track t;
    t.sourceId = std::move(id);
    t.durationSeconds = duration;
    t.cues = std::move(cues);
    return t;
}

int main()
{
    CuePoint hotCueSlot1{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"};

    // Stick track has no cues, local backup does -> merge is everything the
    // backup has, proposed as a candidate.
    {
        Track stickTrack = makeTrack("s1", 200.0, {});
        Track localTrack = makeTrack("l1", 200.0, {hotCueSlot1});
        std::vector<std::pair<const Track *, const Track *>> matches = {{&stickTrack, &localTrack}};

        auto candidates = LocalRestorePlanner::plan(matches);
        assert(candidates.size() == 1);
        assert(candidates[0].stickTrack.sourceId == "s1");
        assert(candidates[0].localTrack.sourceId == "l1");
        assert(candidates[0].mergedCues.size() == 1);
        std::cout << "case 1 (stick empty, local has cues -> merge is local's cues) OK\n";
    }

    // Stick has hot cue slot 1, local backup only offers a DIFFERENT hot
    // cue slot (2) -> merges in the new slot, keeping slot 1 exactly as it
    // is, and is proposed since something new was actually added.
    {
        Track stickTrack = makeTrack("s1", 200.0, {hotCueSlot1});
        CuePoint hotCueSlot2{CuePoint::Kind::Hot, 2, 5000.0, "#00FF00", "break"};
        Track localTrack = makeTrack("l1", 200.0, {hotCueSlot2});
        std::vector<std::pair<const Track *, const Track *>> matches = {{&stickTrack, &localTrack}};

        auto candidates = LocalRestorePlanner::plan(matches);
        assert(candidates.size() == 1);
        assert(candidates[0].mergedCues.size() == 2);
        assert(candidates[0].mergedCues[0].hotCueNumber == 1);  // stick's own cue kept, unchanged
        assert(candidates[0].mergedCues[0].comment == "drop");
        assert(candidates[0].mergedCues[1].hotCueNumber == 2);  // local's cue added
        std::cout << "case 2 (different hot cue slots merge, stick's own cue untouched) OK\n";
    }

    // Stick already has a cue in the slot the local backup would fill ->
    // that slot is never overwritten, so with nothing else to add, no
    // candidate is proposed at all.
    {
        Track stickTrack = makeTrack("s1", 200.0, {hotCueSlot1});
        CuePoint differentSlot1{CuePoint::Kind::Hot, 1, 9000.0, "#0000FF", "different position, same slot"};
        Track localTrack = makeTrack("l1", 200.0, {differentSlot1});
        std::vector<std::pair<const Track *, const Track *>> matches = {{&stickTrack, &localTrack}};

        auto candidates = LocalRestorePlanner::plan(matches);
        assert(candidates.empty());
        std::cout << "case 3 (occupied hot cue slot is never overwritten -> not proposed) OK\n";
    }

    // Memory cues merge by position proximity, not slot number: a local
    // memory cue near an existing one is skipped (avoids near-duplicate
    // clutter); a genuinely new position is added.
    {
        CuePoint existingMemory{CuePoint::Kind::Memory, 0, 10000.0, "", "verse"};
        Track stickTrack = makeTrack("s1", 200.0, {existingMemory});

        CuePoint nearDuplicate{CuePoint::Kind::Memory, 0, 10200.0, "", "verse (rescanned)"};
        CuePoint genuinelyNew{CuePoint::Kind::Memory, 0, 60000.0, "", "chorus"};
        Track localTrack = makeTrack("l1", 200.0, {nearDuplicate, genuinelyNew});
        std::vector<std::pair<const Track *, const Track *>> matches = {{&stickTrack, &localTrack}};

        auto candidates = LocalRestorePlanner::plan(matches);
        assert(candidates.size() == 1);
        assert(candidates[0].mergedCues.size() == 2);  // existing + genuinelyNew, nearDuplicate skipped
        std::cout << "case 4 (memory cues merge by position tolerance) OK\n";
    }

    // Neither side has cues -> nothing to propose.
    {
        Track stickTrack = makeTrack("s1", 200.0, {});
        Track localTrack = makeTrack("l1", 200.0, {});
        std::vector<std::pair<const Track *, const Track *>> matches = {{&stickTrack, &localTrack}};

        auto candidates = LocalRestorePlanner::plan(matches);
        assert(candidates.empty());
        std::cout << "case 5 (neither side has cues -> not proposed) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
