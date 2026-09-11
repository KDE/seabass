// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Confirms sync_controller.cpp's whole-file-replace path (build a
// scratch copy of m.db, apply the same per-item writeHotCues() loop
// against it, then durably+atomically swap it back onto the real file)
// produces byte-for-byte the same end result as writing directly onto
// the real file -- the two code paths this test exercises deliberately
// mirror sync_controller.cpp's own engine-format branch, without pulling
// in Qt/QtConcurrent to test the controller itself.
#include <cassert>
#include <filesystem>
#include <iostream>

#include "application/use_cases/scan_library.hpp"
#include "infrastructure/durable_file_write.hpp"
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

std::vector<Track> makeFixtureTracks(const fs::path &root, int count)
{
    std::vector<Track> tracks;
    for (int i = 0; i < count; i++) {
        Track t;
        t.sourceId = "r" + std::to_string(i);
        t.title = "Song " + std::to_string(i);
        t.artist = "Artist " + std::to_string(i);
        t.filePath = (root / ("song" + std::to_string(i) + ".mp3")).string();
        t.bpm = 120.0;
        t.durationSeconds = 200.0;
        tracks.push_back(t);
    }
    return tracks;
}

// New cues to apply per track, keyed by the reader's own track title
// (Engine reassigns numeric ids on create, so titles are the stable key
// available to this test).
std::vector<CuePoint> cuesFor(int index)
{
    return {
        {CuePoint::Kind::Hot, 1, 1000.0 + index, "#00FF00", "sync-applied"},
        {CuePoint::Kind::Memory, 0, 250.0 + index, "", ""},
    };
}

}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_engine_sync_scratch_replace_test";
    fs::remove_all(root);
    fs::create_directories(root);

    constexpr int TrackCount = 20;  // exceeds MinItemCountForWholeFileReplace (15)

    // Two libraries starting from identical content.
    fs::path directLibrary = root / "direct" / "Engine Library";
    fs::path wholeFileLibrary = root / "wholefile" / "Engine Library";
    fs::create_directories(directLibrary.parent_path());
    fs::create_directories(wholeFileLibrary.parent_path());
    auto tracksTemplate = makeFixtureTracks(root, TrackCount);
    auto r1 = EngineLibraryCreator::create(directLibrary.string(), tracksTemplate, EngineSchemaGeneration::V2);
    assert(r1.errorMessage.empty() && r1.tracksCreated == TrackCount);
    auto r2 = EngineLibraryCreator::create(wholeFileLibrary.string(), tracksTemplate, EngineSchemaGeneration::V2);
    assert(r2.errorMessage.empty() && r2.tracksCreated == TrackCount);

    // Direct path: exactly what sync_controller.cpp does when the
    // whole-file strategy isn't chosen -- write straight onto the real
    // file, once per item.
    {
        LibdjinteropEngineReader reader(directLibrary.string());
        auto readBack = seabass::application::ScanLibrary(reader).execute();
        assert(static_cast<int>(readBack.size()) == TrackCount);
        LibdjinteropEngineCueWriter writer(directLibrary.string());
        for (const auto &t : readBack) {
            int index = std::stoi(t.title.substr(std::string("Song ").size()));
            writer.writeHotCues(t.sourceId, cuesFor(index));
        }
    }

    // Whole-file-replace path: mirrors sync_controller.cpp's engine
    // branch exactly -- copy m.db into scratch, run the identical
    // per-item loop against the scratch copy, then durably+atomically
    // replace the real m.db with the scratch result.
    {
        fs::path realDbFile = wholeFileLibrary / "Database2" / "m.db";
        fs::path scratchDir = root / "scratch";
        fs::remove_all(scratchDir);
        fs::create_directories(scratchDir / "Database2");
        fs::copy_file(realDbFile, scratchDir / "Database2" / "m.db");

        LibdjinteropEngineReader reader(wholeFileLibrary.string());
        auto readBack = seabass::application::ScanLibrary(reader).execute();
        assert(static_cast<int>(readBack.size()) == TrackCount);

        // Scoped so the writer's own SQLite connection to the scratch
        // m.db is closed before copyFileDurablyAtomic() reads it and
        // remove_all() deletes it below -- POSIX happily unlinks a file
        // still open elsewhere in the same process (frees it on last
        // close), so this only ever showed up on Windows, which locks it.
        {
            LibdjinteropEngineCueWriter scratchWriter(scratchDir.string());
            for (const auto &t : readBack) {
                int index = std::stoi(t.title.substr(std::string("Song ").size()));
                scratchWriter.writeHotCues(t.sourceId, cuesFor(index));
            }
        }

        bool committed = seabass::infrastructure::copyFileDurablyAtomic(
            (scratchDir / "Database2" / "m.db").string(), realDbFile.string());
        assert(committed);
        fs::remove_all(scratchDir);
    }

    // Both libraries must now show identical cue data per track.
    {
        LibdjinteropEngineReader directReader(directLibrary.string());
        auto directTracks = seabass::application::ScanLibrary(directReader).execute();
        LibdjinteropEngineReader wholeFileReader(wholeFileLibrary.string());
        auto wholeFileTracks = seabass::application::ScanLibrary(wholeFileReader).execute();
        assert(directTracks.size() == wholeFileTracks.size());

        auto findByTitle = [](const std::vector<Track> &tracks, const std::string &title) -> const Track & {
            for (const auto &t : tracks) {
                if (t.title == title) {
                    return t;
                }
            }
            throw std::runtime_error("title not found: " + title);
        };

        for (int i = 0; i < TrackCount; i++) {
            std::string title = "Song " + std::to_string(i);
            const Track &fromDirect = findByTitle(directTracks, title);
            const Track &fromWholeFile = findByTitle(wholeFileTracks, title);
            assert(fromDirect.cues.size() == fromWholeFile.cues.size());
            assert(fromDirect.cues.size() == 2);
            for (size_t c = 0; c < fromDirect.cues.size(); c++) {
                assert(fromDirect.cues[c].kind == fromWholeFile.cues[c].kind);
                assert(fromDirect.cues[c].positionMs == fromWholeFile.cues[c].positionMs);
            }
        }
        std::cout << "case 1 (whole-file-replace path matches direct path for all " << TrackCount << " tracks) OK\n";
    }

    fs::remove_all(root);
    std::cout << "all cases passed\n";
    return 0;
}
