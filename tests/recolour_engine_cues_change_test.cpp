// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// RecolourEngineCuesChange through the real save loop: an Engine track
// whose pads carry alpha 0 (what the cue writer wrote before 05d71bbd)
// reads back with the pad's default colour, and nothing else about it
// moves: the coloured pad, the loop's length, the cue point.

#include <QString>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>

#include <djinterop/djinterop.hpp>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/hidden_engine_cues.hpp"
#include "gui/edit/changes/recolour_engine_cues_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "gui/qt_path.hpp"
#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

using namespace seabass;
using namespace seabass::gui;
namespace fs = std::filesystem;

namespace
{

std::vector<domain::Track> engineTracks(const fs::path &library)
{
    return infrastructure::engine::LibdjinteropEngineReader(pathToUtf8(library)).readAll();
}

}  // namespace

int main()
{
    const fs::path stick = testing::scratchRoot() / "seabass_recolour_engine_cues_change_test";
    fs::remove_all(stick);
    const fs::path library = stick / "Engine Library";
    fs::create_directories(stick);
    // A stick root with no PIONEER folder: the save context is told the
    // Engine path alone.
    auto db = djinterop::engine::create_database(pathToUtf8(library));
    djinterop::track_snapshot snapshot;
    snapshot.title = "Hidden";
    snapshot.relative_path = "hidden.mp3";
    snapshot.sample_rate = 44100.0;
    auto track = db.create_track(snapshot);
    const int64_t trackId = track.id();

    // What a pre-05d71bbd writer left: pads 2 and 5 with alpha 0, pad 1
    // with a real colour, a loop on pad 4 with alpha 0, a cue point.
    std::vector<std::optional<djinterop::hot_cue>> pads(8);
    pads[0] = djinterop::hot_cue{"", 44100.0 * 10, djinterop::pad_color{0x12, 0x34, 0x56, 0xFF}};
    pads[1] = djinterop::hot_cue{"", 44100.0 * 20, djinterop::pad_color{0, 0, 0, 0}};
    pads[4] = djinterop::hot_cue{"", 44100.0 * 50, djinterop::pad_color{0, 0, 0, 0}};
    track.set_hot_cues(pads);
    std::vector<std::optional<djinterop::loop>> loops(8);
    loops[3] = djinterop::loop{"", 44100.0 * 40, 44100.0 * 44, djinterop::pad_color{0, 0, 0, 0}};
    track.set_loops(loops);
    track.set_main_cue(44100.0 * 5);

    // The finder sees three hidden pads: two cues and a loop.
    const auto before = engineTracks(library);
    assert(before.size() == 1);
    const auto hidden = domain::HiddenEngineCueFinder::find(before);
    assert(hidden.size() == 1 && hidden[0].hotCues == 2 && hidden[0].loops == 1);

    // Through the save loop, as Library Health stages it.
    application::CancellationToken token;
    SaveContext ctx(token, application::NullProgressReporter::instance(), nullptr, {}, pathToQString(library));
    std::vector<std::shared_ptr<PendingChange>> changes;
    changes.push_back(std::make_shared<RecolourEngineCuesChange>(pathToQString(library), hidden[0], 1));
    const SaveLoopResult result = runSaveLoop(changes, ctx);
    assert(result.error.isEmpty() && !result.cancelled);
    assert(result.appliedIds.size() == 1);

    // Nothing hidden any more, and the pads say the defaults.
    assert(domain::HiddenEngineCueFinder::find(engineTracks(library)).empty());
    auto after = djinterop::engine::load_database(pathToUtf8(library)).track_by_id(trackId);
    assert(after.has_value());
    auto cues = after->hot_cues();
    assert(cues[0] && cues[0]->color.a == 0xFF && cues[0]->color.r == 0x12 && cues[0]->color.g == 0x34
           && "a pad that had a colour keeps it");
    assert(cues[1] && cues[1]->color.a == 0xFF && cues[1]->color.r == 0xEF && "pad 2: Engine's orange");
    assert(cues[4] && cues[4]->color.a == 0xFF && "pad 5: a colour, whichever");
    assert(std::abs(cues[1]->sample_offset - 44100.0 * 20) < 1.0 && "positions unchanged");
    auto afterLoops = after->loops();
    assert(afterLoops[3] && afterLoops[3]->color.a == 0xFF && afterLoops[3]->color.r == 0xCE && "pad 4: Engine's red");
    assert(std::abs(afterLoops[3]->end_sample_offset - 44100.0 * 44) < 1.0 && "the loop keeps its length");
    auto main = after->main_cue();
    assert(main && std::abs(*main - 44100.0 * 5) < 1.0 && "the cue point is where it was");
    std::cout << "case 1 (hidden pads take the pad's default colour, nothing else moves) OK\n";

    // Not the throwing overload: the save context and the library's
    // handles above are still open, and Windows will not delete an open
    // file, so the test aborted here with nothing said. The next run
    // clears what is left before it starts.
    std::error_code cleanupError;
    fs::remove_all(stick, cleanupError);
    std::cout << "all cases passed\n";
    return 0;
}
