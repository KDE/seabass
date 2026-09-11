// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Times a real staged save against a real stick.
//
// This is the measurement the whole optimisation round exists for. The
// corpus runner's work counts say the repeated work is gone; they cannot
// say what that is worth in seconds, because seconds are a property of the
// medium (docs/write-path-performance.md). Only a stick can answer that.
//
// The workload is the one that started it: remove a 0:00 memory cue from N
// tracks through the real save loop, staged exactly as the Library Health
// page stages them. 201 of those took 155 s on RV2 on 2026-09-07.
//
// It never touches the real library. Everything happens inside
// <stick>/.seabass-savebench/, which it creates and removes. The setup
// phase, which copies the catalog onto the stick and puts the stray cues
// there to be removed, is deliberately outside the timed region.
//
// Usage:
//   staged_save_bench <stick-mount-point> [item-count]

#include <QCoreApplication>
#include <QString>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/progress_reporter.hpp"
#include "application/use_cases/scan_library.hpp"
#include "gui/edit/changes/remove_junk_cue_change.hpp"
#include "gui/edit/save_context.hpp"
#include "gui/edit/save_loop.hpp"
#include "infrastructure/rekordbox/kaitai_rekordbox_reader.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/rekordbox_cue_writer.hpp"
#include "infrastructure/work_counters.hpp"

namespace fs = std::filesystem;
using namespace seabass;

namespace
{

double secondsSince(const std::chrono::steady_clock::time_point &start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// Reported alongside every run because it is a candidate explanation for a
// curve that bends: allocation on a nearly full exFAT volume gets slower
// as free space fragments, and a run that fills the medium further as it
// goes would look superlinear for reasons that have nothing to do with the
// code under test.
double freeSpaceGiB(const fs::path &path)
{
    std::error_code ec;
    const auto info = fs::space(path, ec);
    if (ec) {
        return -1.0;
    }
    return static_cast<double>(info.available) / (1024.0 * 1024.0 * 1024.0);
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::cerr << "usage: staged_save_bench <stick-mount-point> [item-count]\n";
        return 1;
    }
    const fs::path stick = argv[1];
    const int items = argc > 2 ? std::atoi(argv[2]) : 50;

    std::error_code ec;
    if (!fs::is_directory(stick / "PIONEER", ec)) {
        std::cerr << stick << " has no PIONEER folder\n";
        return 1;
    }

    const fs::path scratch = stick / ".seabass-savebench";
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    const fs::path root = scratch / "PIONEER";

    std::cout << "Copying the catalog onto the stick (not timed)...\n" << std::flush;
    auto copyStart = std::chrono::steady_clock::now();
    fs::copy(stick / "PIONEER", root, fs::copy_options::recursive, ec);
    if (ec) {
        std::cerr << "copy failed: " << ec.message() << "\n";
        fs::remove_all(scratch, ec);
        return 1;
    }
    std::cout << "  copied in " << secondsSince(copyStart) << " s\n" << std::flush;

    infrastructure::rekordbox::KaitaiRekordboxReader reader(root.string());
    auto tracks = application::ScanLibrary(reader).execute();
    std::cout << "  " << tracks.size() << " tracks in the scratch catalog\n" << std::flush;

    // Setup: give the first `items` tracks a 0:00 memory cue to remove.
    // Not timed -- this is building the workload, not measuring it.
    std::cout << "Planting " << items << " stray cues (not timed)...\n" << std::flush;
    auto plantStart = std::chrono::steady_clock::now();
    std::vector<domain::Track> targets;
    {
        // Indexed, so building the workload does not dominate the run.
        // Timed anyway and reported, because an unindexed writer here is
        // exactly the shape the code had before this round.
        infrastructure::rekordbox::AnlzPathIndex plantIndex(root.string());
        infrastructure::rekordbox::RekordboxCueWriter writer(root.string(), &plantIndex);
        for (const auto &track : tracks) {
            if (static_cast<int>(targets.size()) >= items) {
                break;
            }
            domain::CuePoint stray;
            stray.kind = domain::CuePoint::Kind::Memory;
            stray.positionMs = 0.0;
            std::vector<domain::CuePoint> withStray = track.cues;
            withStray.push_back(stray);
            try {
                writer.writeHotCues(track.sourceId, withStray);
            } catch (const std::exception &) {
                continue;  // no analysis file for this row
            }
            domain::Track planted = track;
            planted.cues = withStray;
            targets.push_back(planted);
        }
    }
    const double plantSeconds = secondsSince(plantStart);
    std::cout << "  planted " << targets.size() << " in " << plantSeconds << " s ("
              << (plantSeconds / static_cast<double>(targets.size())) * 1000.0 << " ms/item)\n"
              << "  (indexed, and with no backup, so this is not the save being measured)\n"
              << std::flush;
    if (targets.empty()) {
        fs::remove_all(scratch, ec);
        return 1;
    }

    // The measurement: stage every removal and run the one save loop the
    // app runs, against files on the stick.
    std::vector<std::shared_ptr<gui::PendingChange>> changes;
    for (const auto &track : targets) {
        changes.push_back(
            std::make_shared<gui::RemoveJunkCueChange>(QString::fromStdString(root.string()), track));
    }

    infrastructure::WorkCounters::instance().reset();
    application::CancellationToken cancel;
    gui::SaveContext ctx(cancel, application::NullProgressReporter::instance(), nullptr,
                         QString::fromStdString(root.string()), QString());

    std::cout << "Saving " << changes.size() << " staged removals...\n" << std::flush;
    auto saveStart = std::chrono::steady_clock::now();
    auto result = runSaveLoop(changes, ctx);
    const double seconds = secondsSince(saveStart);
    const auto counts = infrastructure::WorkCounters::instance().snapshot();

    std::cout << "\n=== " << result.appliedIds.size() << " of " << changes.size() << " applied ===\n";
    if (!result.error.isEmpty()) {
        std::cout << "error: " << result.error.toStdString() << "\n";
    }
    std::cout << "  wall clock:     " << seconds << " s\n";
    std::cout << "  free space:     " << freeSpaceGiB(stick) << " GiB left on the medium\n";
    std::cout << "  per item:       " << (seconds / static_cast<double>(changes.size())) * 1000.0 << " ms\n";
    std::cout << "  work:           " << counts.describe() << "\n";

    // Verify it actually did the job, so a fast run cannot be a run that
    // quietly wrote nothing.
    // Only on the tracks this run touched: the library has stray cues of
    // its own that nothing here staged, and counting those would report a
    // correct save as a failed one.
    std::set<std::string> touched;
    for (const auto &track : targets) {
        touched.insert(track.sourceId);
    }
    infrastructure::rekordbox::KaitaiRekordboxReader rereader(root.string());
    auto after = application::ScanLibrary(rereader).execute();
    int strayLeft = 0;
    for (const auto &track : after) {
        if (touched.count(track.sourceId) == 0) {
            continue;
        }
        for (const auto &cue : track.cues) {
            if (cue.kind == domain::CuePoint::Kind::Memory && cue.positionMs == 0.0) {
                ++strayLeft;
            }
        }
    }
    std::cout << "  stray cues left on the " << touched.size() << " touched tracks: " << strayLeft
              << " (expected 0)\n";

    fs::remove_all(scratch, ec);
    return strayLeft == 0 ? 0 : 1;
}
