// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// #50 through a GUI save: the Engine cue writer a save builds answers a
// file's lead-in from the same checked metadata cache the catalog read
// the cues through, so a cue the save did not touch is written back
// exactly where it was.
//
// The stick's cache holds a lead-in for the file at its current size and
// mtime that is not what the file's start says (an FFmpeg one, 25.06 ms,
// on a LAME file, 51.18 ms). The catalog reads the track's hot cue
// through it; Add Cue then rewrites the track's whole cue set. A writer
// reading the file's start on its own takes 51.18 ms off a cue the
// reader added 25.06 ms to, and the untouched cue moves 26 ms early.
//
// Usage: engine_save_lead_in_test FIXTURE_DIR (tests/fixtures/mp3-lead-in)

#include <QString>

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

#include <djinterop/djinterop.hpp>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/matching_policy.hpp"
#include "domain/track.hpp"
#include "gui/edit/changes/add_cue_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/engine/libdjinterop_engine_library_creator.hpp"
#include "infrastructure/local/metadata_cache.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using namespace seabass::gui;
using seabass::domain::CuePoint;
using seabass::domain::Track;
namespace fs = std::filesystem;

int main(int argc, char **argv)
{
    if (argc < 2 || !fs::is_directory(argv[1])) {
        std::cerr << "usage: engine_save_lead_in_test FIXTURE_DIR (tests/fixtures/mp3-lead-in)\n";
        return 2;
    }
    const fs::path fixtures = seabass::pathFromUtf8(argv[1]);
    seabass::domain::MatchingPolicy::reset();
    assert(seabass::domain::MatchingPolicy::correctMp3CueTiming());

    const fs::path stick = seabass::testing::scratchRoot() / "seabass_engine_save_lead_in_test";
    fs::remove_all(stick);
    const fs::path library = stick / "Engine Library";
    fs::create_directories(library);
    const fs::path file = library / "song.mp3";
    fs::copy_file(fixtures / "lame-cbr-44100.mp3", file);

    // A hot cue at 5000 ms, stored by Create Engine Library with the
    // file's own 51.18 ms: (5000 - 51.18) * 44.1 samples.
    Track t;
    t.sourceId = "r0";
    t.title = "Lead In";
    t.artist = "Artist";
    t.filePath = seabass::pathToUtf8(file);
    t.bpm = 120.0;
    t.durationSeconds = 300.0;
    t.cues = {CuePoint{CuePoint::Kind::Hot, 1, 5000.0, "#ff0000", "drop"}};
    const auto created = seabass::infrastructure::engine::EngineLibraryCreator::create(
        seabass::pathToUtf8(library), {t}, seabass::infrastructure::engine::EngineSchemaGeneration::V2);
    assert(created.errorMessage.empty() && created.tracksCreated == 1);

    auto rawHotCue = [&](int pad) {
        auto db = djinterop::engine::load_database(seabass::pathToUtf8(library));
        const auto tracks = db.tracks();
        assert(tracks.size() == 1);
        const auto hot = tracks.front().hot_cues();
        assert(hot.size() >= static_cast<std::size_t>(pad) && hot[pad - 1]);
        return std::make_pair(std::to_string(tracks.front().id()), hot[pad - 1]->sample_offset);
    };
    const auto [sourceId, before] = rawHotCue(1);

    // The stick's metadata cache says the file is an FFmpeg one.
    {
        const auto lavc =
            seabass::infrastructure::audio::readMp3LeadIn(seabass::pathToUtf8(fixtures / "ffmpeg-lavc-44100.mp3"));
        assert(lavc && lavc->tag == seabass::domain::Mp3LeadIn::Tag::Lavc);
        seabass::infrastructure::local::MetadataCache cache(seabass::pathToUtf8(stick));
        seabass::application::FileMetadata entry;
        entry.durationSeconds = 3.0;
        entry.sampleRate = 44100;
        entry.mp3LeadInRead = true;
        entry.mp3LeadIn = lavc;
        cache.store(seabass::pathToUtf8(file), entry);
        assert(cache.save());
    }

    auto &noProgress = seabass::application::NullProgressReporter::instance();
    seabass::application::CancellationToken token;
    const QString root = pathToQString(library);
    SaveContext ctx(token, noProgress, {}, {}, root);
    std::vector<std::shared_ptr<PendingChange>> changes = {std::make_shared<AddCueChange>(
        QStringLiteral("engine"), root, QString::fromStdString(sourceId), 1234.0, QStringLiteral("hot"), 2,
        QStringLiteral("#00FF00"), QString(), false, 0.0, QStringLiteral("Lead In"))};
    const SaveLoopResult result = runSaveLoop(changes, ctx);
    if (!result.error.isEmpty()) {
        std::cerr << "save failed: " << result.error.toStdString() << "\n";
    }
    assert(result.error.isEmpty() && result.appliedIds.size() == 1);

    const auto [sameId, after] = rawHotCue(1);
    std::cout << "hot cue 1 before " << before << ", after " << after << " samples\n";
    assert(std::abs(after - before) < 1e-3 && "the cue the save did not touch stays where it was");
    // And the new one is moved by the cache's 1105 samples too.
    const auto [newId, added] = rawHotCue(2);
    assert(std::abs(added - (1234.0 * 44.1 - 1105.0)) < 1e-3);
    (void)sameId;
    (void)newId;

    fs::remove_all(stick);
    std::cout << "engine_save_lead_in_test passed\n";
    return 0;
}
