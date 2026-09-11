// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Confirms LibdjinteropEngineCueWriter's new hot-loop write path
// (set_loops(), alongside set_hot_cues()) round-trips correctly through
// a real (scratch) Engine library via libdjinterop, and that a slot
// switching from a hot cue to a hot loop (or back) is cleared on its old
// side -- the "a slot is either a cue or a loop, never both" rule this
// whole feature leans on, enforced at the write layer.
#include <cassert>
#include <filesystem>
#include <iostream>

#include "application/use_cases/scan_library.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"
#include "infrastructure/engine/libdjinterop_engine_library_creator.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::engine;
using seabass::domain::CuePoint;
using seabass::domain::Track;
namespace fs = std::filesystem;

namespace
{

fs::path freshRoot(const std::string &caseName)
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_engine_loop_cue_test" / caseName / "Engine Library";
    fs::remove_all(root.parent_path());
    fs::create_directories(root.parent_path());
    return root;
}

std::vector<Track> oneTrackTemplate(const fs::path &root)
{
    Track t;
    t.sourceId = "r0";
    t.title = "Loop Song";
    t.artist = "Artist";
    t.filePath = (root / "song.mp3").string();
    t.bpm = 128.0;
    t.durationSeconds = 300.0;
    return {t};
}

const Track *findByTitle(const std::vector<Track> &tracks, const std::string &title)
{
    for (const auto &t : tracks) {
        if (t.title == title) {
            return &t;
        }
    }
    return nullptr;
}

const CuePoint *findBySlot(const std::vector<CuePoint> &cues, int slot)
{
    for (const auto &c : cues) {
        if (c.kind == CuePoint::Kind::Hot && c.hotCueNumber == slot) {
            return &c;
        }
    }
    return nullptr;
}

}  // namespace

int main()
{
    // Case 1: a plain hot cue in slot 1 and a hot loop in slot 2 both
    // round-trip with their own real fields intact.
    {
        fs::path root = freshRoot("case1");
        auto created = EngineLibraryCreator::create(root.string(), oneTrackTemplate(root), EngineSchemaGeneration::V2);
        assert(created.errorMessage.empty() && created.tracksCreated == 1);

        LibdjinteropEngineReader reader(root.string());
        auto tracks = seabass::application::ScanLibrary(reader).execute();
        const Track *track = findByTitle(tracks, "Loop Song");
        assert(track != nullptr);

        CuePoint hotCue{CuePoint::Kind::Hot, 1, 5000.0, "#ff0000", "drop"};
        CuePoint hotLoop{CuePoint::Kind::Hot, 2, 10000.0, "#00ff00", "groove loop"};
        hotLoop.isLoop = true;
        hotLoop.loopEndMs = 14000.0;

        LibdjinteropEngineCueWriter writer(root.string());
        writer.writeHotCues(track->sourceId, {hotCue, hotLoop});

        LibdjinteropEngineReader readerAfter(root.string());
        auto tracksAfter = seabass::application::ScanLibrary(readerAfter).execute();
        const Track *after = findByTitle(tracksAfter, "Loop Song");
        assert(after != nullptr);

        const CuePoint *slot1 = findBySlot(after->cues, 1);
        assert(slot1 != nullptr && !slot1->isLoop);
        assert(slot1->positionMs == 5000.0);
        assert(slot1->comment == "drop");

        const CuePoint *slot2 = findBySlot(after->cues, 2);
        assert(slot2 != nullptr && slot2->isLoop);
        assert(slot2->positionMs == 10000.0);
        assert(slot2->loopEndMs == 14000.0);
        assert(slot2->comment == "groove loop");

        std::cout << "case 1 (hot cue + hot loop round-trip with real fields) OK\n";
    }

    // Case 2: slot 1 starts as a hot cue, then gets overwritten with a
    // hot loop the same way AddCueController does it (re-read, drop the
    // existing same-slot entry, add the new one, write the whole list
    // back) -- the old hot cue must not survive alongside the new loop.
    {
        fs::path root = freshRoot("case2");
        auto created = EngineLibraryCreator::create(root.string(), oneTrackTemplate(root), EngineSchemaGeneration::V2);
        assert(created.errorMessage.empty());

        LibdjinteropEngineReader reader(root.string());
        auto tracks = seabass::application::ScanLibrary(reader).execute();
        const Track *track = findByTitle(tracks, "Loop Song");
        assert(track != nullptr);

        LibdjinteropEngineCueWriter writer(root.string());
        writer.writeHotCues(track->sourceId, {CuePoint{CuePoint::Kind::Hot, 1, 5000.0, "#ff0000", "drop"}});

        LibdjinteropEngineReader midReader(root.string());
        auto midTracks = seabass::application::ScanLibrary(midReader).execute();
        const Track *mid = findByTitle(midTracks, "Loop Song");
        assert(mid != nullptr);
        assert(findBySlot(mid->cues, 1) != nullptr && !findBySlot(mid->cues, 1)->isLoop);

        CuePoint loop{CuePoint::Kind::Hot, 1, 20000.0, "#3daee9", "now a loop"};
        loop.isLoop = true;
        loop.loopEndMs = 24000.0;
        writer.writeHotCues(mid->sourceId, {loop});

        LibdjinteropEngineReader finalReader(root.string());
        auto finalTracks = seabass::application::ScanLibrary(finalReader).execute();
        const Track *final_ = findByTitle(finalTracks, "Loop Song");
        assert(final_ != nullptr);

        int slot1Count = 0;
        for (const auto &c : final_->cues) {
            if (c.kind == CuePoint::Kind::Hot && c.hotCueNumber == 1) {
                slot1Count++;
                assert(c.isLoop);
                assert(c.positionMs == 20000.0);
                assert(c.loopEndMs == 24000.0);
            }
        }
        assert(slot1Count == 1);
        std::cout << "case 2 (slot switches from hot cue to hot loop, old cue cleared) OK\n";
    }

    // Case 3: a Kind::Memory cue with isLoop set -- Engine has no
    // memory-loop concept (see writer's own comment), so this must not
    // throw; the position is still eligible as the one memory cue, its
    // loop-out is just dropped.
    {
        fs::path root = freshRoot("case3");
        auto created = EngineLibraryCreator::create(root.string(), oneTrackTemplate(root), EngineSchemaGeneration::V2);
        assert(created.errorMessage.empty());

        LibdjinteropEngineReader reader(root.string());
        auto tracks = seabass::application::ScanLibrary(reader).execute();
        const Track *track = findByTitle(tracks, "Loop Song");
        assert(track != nullptr);

        CuePoint memoryLoop{CuePoint::Kind::Memory, 0, 3000.0, "", ""};
        memoryLoop.isLoop = true;
        memoryLoop.loopEndMs = 7000.0;

        LibdjinteropEngineCueWriter writer(root.string());
        writer.writeHotCues(track->sourceId, {memoryLoop});

        LibdjinteropEngineReader readerAfter(root.string());
        auto tracksAfter = seabass::application::ScanLibrary(readerAfter).execute();
        const Track *after = findByTitle(tracksAfter, "Loop Song");
        assert(after != nullptr);
        assert(after->cues.size() == 1);
        assert(after->cues[0].kind == CuePoint::Kind::Memory);
        assert(after->cues[0].positionMs == 3000.0);
        std::cout << "case 3 (memory cue with isLoop set doesn't throw, position still written) OK\n";
    }

    std::cout << "All libdjinterop_engine_loop_cue_test cases passed.\n";
    return 0;
}
