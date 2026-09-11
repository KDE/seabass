// writeHotCues() takes the COMPLETE replacement set of cues, the same
// contract every CueWriter has. That has to hold for Engine's single
// memory-style cue too: if the incoming set has no memory cue, the track
// has no main cue any more.
//
// It did not. set_main_cue() was only called when a memory cue survived,
// so a caller that removed the last one (Sync copying a rekordbox track
// that has only hot cues, Library Health removing a stray cue, Add Cue
// replacing a set) left the old main cue in the database, and the reader
// handed it straight back as a memory cue that had just been removed.

#include <cassert>
#include <sqlite3.h>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <djinterop/djinterop.hpp>

#include "domain/track.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::engine;
namespace fs = std::filesystem;

namespace
{

// One directory per case: create_database() refuses to create over an
// existing database (same reason the propagate test does this).
fs::path freshRoot(const std::string &caseName)
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_engine_cue_writer_main_cue_test" / caseName / "Engine Library";
    fs::remove_all(root.parent_path());
    fs::create_directories(root.parent_path());
    return root;
}

// The writer falls back to 44.1 kHz when a track has no sample rate, which
// a freshly created snapshot does not, so positions convert with this.
constexpr double SampleRate = 44100.0;

seabass::domain::CuePoint hotCue(int number, double positionMs)
{
    seabass::domain::CuePoint cue;
    cue.kind = seabass::domain::CuePoint::Kind::Hot;
    cue.hotCueNumber = number;
    cue.positionMs = positionMs;
    return cue;
}

seabass::domain::CuePoint memoryCue(double positionMs)
{
    seabass::domain::CuePoint cue;
    cue.kind = seabass::domain::CuePoint::Kind::Memory;
    cue.hotCueNumber = 0;
    cue.positionMs = positionMs;
    return cue;
}

djinterop::track makeTrack(djinterop::database &db)
{
    djinterop::track_snapshot snapshot;
    snapshot.title = "Subject";
    snapshot.relative_path = "subject.mp3";
    return db.create_track(snapshot);
}

}  // namespace

int main()
{
    // A memory cue in the incoming set becomes the main cue.
    {
        fs::path root = freshRoot("case1");
        auto db = djinterop::engine::create_database(root.string());
        auto track = makeTrack(db);
        LibdjinteropEngineCueWriter writer(root.string());

        writer.writeHotCues(std::to_string(track.id()), {hotCue(1, 1000.0), memoryCue(5000.0)});

        auto after = db.track_by_id(track.id());
        assert(after.has_value());
        auto mainCue = after->main_cue();
        assert(mainCue.has_value());
        assert(std::abs(*mainCue - 5.0 * SampleRate) < 1.0);
        std::cout << "case 1 (a memory cue in the set becomes the main cue) OK\n";
    }

    // THE REGRESSION: a set with no memory cue clears the main cue rather
    // than leaving the previous one behind.
    {
        fs::path root = freshRoot("case2");
        auto db = djinterop::engine::create_database(root.string());
        auto track = makeTrack(db);
        LibdjinteropEngineCueWriter writer(root.string());

        writer.writeHotCues(std::to_string(track.id()), {hotCue(1, 1000.0), memoryCue(5000.0)});
        auto seeded = db.track_by_id(track.id());
        assert(seeded.has_value() && seeded->main_cue().has_value());

        // Same track, now written with hot cues only.
        writer.writeHotCues(std::to_string(track.id()), {hotCue(1, 1000.0)});

        auto after = db.track_by_id(track.id());
        assert(after.has_value());
        assert(!after->main_cue().has_value());
        std::cout << "case 2 (no memory cue in the set clears the main cue) OK\n";
    }

    // An empty set clears everything, hot cues included.
    {
        fs::path root = freshRoot("case3");
        auto db = djinterop::engine::create_database(root.string());
        auto track = makeTrack(db);
        LibdjinteropEngineCueWriter writer(root.string());

        writer.writeHotCues(std::to_string(track.id()), {hotCue(1, 1000.0), memoryCue(5000.0)});
        writer.writeHotCues(std::to_string(track.id()), {});

        auto after = db.track_by_id(track.id());
        assert(after.has_value());
        assert(!after->main_cue().has_value());
        auto slots = after->hot_cues();
        for (const auto &slot : slots) {
            assert(!slot.has_value());
        }
        std::cout << "case 3 (an empty set clears the main cue and every hot cue) OK\n";
    }

    // A track whose loops blob does not decode: the reader would report
    // no loops, and a cue write built from that would erase every hot
    // loop. The writer refuses, and the track's cues are untouched.
    {
        fs::path root = freshRoot("case4");
        auto db = djinterop::engine::create_database(root.string());
        auto track = makeTrack(db);
        seabass::infrastructure::engine::LibdjinteropEngineCueWriter writer(root.string());
        writer.writeHotCues(std::to_string(track.id()), {hotCue(1, 1000.0), hotCue(2, 2000.0)});
        {
            sqlite3 *raw = nullptr;
            assert(sqlite3_open((root / "Database2" / "m.db").string().c_str(), &raw) == SQLITE_OK);
            char *err = nullptr;
            // One byte: no valid loops blob has that length.
            assert(sqlite3_exec(raw, ("UPDATE PerformanceData SET loops = X'00' WHERE trackId = "
                                      + std::to_string(track.id())).c_str(),
                                nullptr, nullptr, &err) == SQLITE_OK);
            sqlite3_close(raw);
        }
        bool refused = false;
        try {
            writer.writeHotCues(std::to_string(track.id()), {hotCue(3, 3000.0)});
        } catch (const std::runtime_error &e) {
            refused = std::string(e.what()).find("refusing") != std::string::npos;
        }
        assert(refused);
        auto reread = djinterop::engine::load_database(root.string()).track_by_id(track.id());
        assert(reread.has_value());
        auto cues = reread->hot_cues();
        assert(cues.size() >= 2 && cues[0].has_value() && cues[1].has_value() && !cues[2].has_value());
        std::cout << "case 4 (an unreadable loops blob refuses the write instead of erasing loops) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
