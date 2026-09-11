// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What one Engine cue write costs, and how much of that is the database
// being reopened for every single item.
//
// This is the number the stray-cue investigation never measured, and it is
// the one that matters most for Sync: Sync's Engine target goes through
// LibdjinteropEngineCueWriter::writeHotCues(), which calls
// djinterop::engine::load_database() at the top of every call and then
// issues three separate auto-commit updates (hot cues, loops, main cue).
//
// Three shapes are timed, on the same data:
//   1. today, on the stick        reopen per item, three commits per item
//   2. today, on a ramdisk copy   the same, with the I/O taken out, so the
//                                 split between CPU and device is visible
//   3. proposed                   one database handle held for the whole
//                                 run, one track::update(snapshot) per item
//
// SAFETY: the real Engine Library is only ever read. Everything is done on
// copies under <stick>/.seabass-writebench-engine/ and in the system temp
// directory, both removed afterwards.
//
// Build:
//   g++ -std=c++23 -O2 -I src -I third_party/libdjinterop/include \
//       tools/engine_write_bench.cpp -o build/engine_write_bench \
//       build/libseabass_core.a build/libDjInterop.a ... -lz -ldl -lpthread
// (see docs/write-path-performance.md for the exact line)

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <djinterop/djinterop.hpp>

#include "domain/track.hpp"
#include "infrastructure/engine/libdjinterop_engine_cue_writer.hpp"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using seabass::infrastructure::engine::LibdjinteropEngineCueWriter;

namespace
{

double seconds(Clock::time_point t)
{
    return std::chrono::duration<double>(Clock::now() - t).count();
}

// A plausible replacement cue set: what Sync copies onto a target track.
std::vector<seabass::domain::CuePoint> cueSet()
{
    std::vector<seabass::domain::CuePoint> cues;
    for (int i = 1; i <= 4; ++i) {
        seabass::domain::CuePoint cue;
        cue.kind = seabass::domain::CuePoint::Kind::Hot;
        cue.hotCueNumber = i;
        cue.positionMs = 1000.0 * i;
        cues.push_back(cue);
    }
    seabass::domain::CuePoint memory;
    memory.kind = seabass::domain::CuePoint::Kind::Memory;
    memory.positionMs = 500.0;
    cues.push_back(memory);
    return cues;
}

void row(const std::string &label, double s, int items)
{
    std::cout << "  " << std::left << std::setw(50) << label << std::right << std::setw(8) << std::fixed
              << std::setprecision(2) << s << " s" << std::setw(10) << std::setprecision(1) << (s * 1000.0 / items)
              << " ms/item\n";
}

std::vector<std::int64_t> firstTrackIds(const std::string &root, int wanted)
{
    auto db = djinterop::engine::load_database(root);
    std::vector<std::int64_t> ids;
    for (const auto &track : db.tracks()) {
        if (static_cast<int>(ids.size()) >= wanted) {
            break;
        }
        ids.push_back(track.id());
    }
    return ids;
}

// Today's shape: the writer reopens the database on every call.
double timeReopenPerItem(const std::string &root, const std::vector<std::int64_t> &ids, int &failed)
{
    LibdjinteropEngineCueWriter writer(root);
    const auto cues = cueSet();
    failed = 0;
    auto t0 = Clock::now();
    for (std::int64_t id : ids) {
        try {
            writer.writeHotCues(std::to_string(id), cues);
        } catch (const std::exception &) {
            ++failed;
        }
    }
    return seconds(t0);
}

// Proposed: one handle for the whole save, and one transaction per item
// instead of three, by mutating the snapshot and writing it back once.
double timeHeldHandle(const std::string &root, const std::vector<std::int64_t> &ids, int &failed)
{
    failed = 0;
    auto t0 = Clock::now();
    auto db = djinterop::engine::load_database(root);
    for (std::int64_t id : ids) {
        auto track = db.track_by_id(id);
        if (!track) {
            continue;
        }
        double sampleRate = 44100.0;
        try {
            if (auto rate = track->sample_rate()) {
                sampleRate = *rate;
            }
        } catch (const std::exception &) {
            // same fallback the real writer uses
        }
        // Not defensive padding: on this real library snapshot() throws
        // for tracks whose data blob libdjinterop cannot decode, the same
        // quirk the cue writer already works around for sample_rate().
        // That is a real obstacle to the "one update() per item" shape,
        // not a benchmark artefact, so it is counted rather than hidden.
        djinterop::track_snapshot snapshot;
        try {
            snapshot = track->snapshot();
        } catch (const std::exception &) {
            ++failed;
            continue;
        }
        std::vector<std::optional<djinterop::hot_cue>> slots(8);
        for (int i = 0; i < 4; ++i) {
            slots[i] = djinterop::hot_cue{"", 1000.0 * (i + 1) / 1000.0 * sampleRate, djinterop::engine::standard_pad_colors::pad_1};
        }
        snapshot.hot_cues = slots;
        snapshot.main_cue = 0.5 * sampleRate;
        try {
            track->update(snapshot);
        } catch (const std::exception &) {
            ++failed;
        }
    }
    return seconds(t0);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: engine_write_bench <stick-mount-point> [items]\n";
        return 1;
    }
    const fs::path stick = argv[1];
    const int items = argc > 2 ? std::atoi(argv[2]) : 50;
    const fs::path realEngine = stick / "Engine Library";
    if (!fs::is_directory(realEngine)) {
        std::cerr << "no Engine Library at " << realEngine << "\n";
        return 1;
    }

    const fs::path onStick = stick / ".seabass-writebench-engine";
    const fs::path onRam = fs::temp_directory_path() / "seabass-writebench-engine";
    for (const auto &dir : {onStick, onRam}) {
        fs::remove_all(dir);
        fs::create_directories(dir);
        fs::copy(realEngine, dir / "Engine Library", fs::copy_options::recursive);
    }
    std::uintmax_t bytes = 0;
    for (const auto &entry : fs::recursive_directory_iterator(onRam / "Engine Library")) {
        if (entry.is_regular_file()) {
            bytes += entry.file_size();
        }
    }
    std::cout << "Engine Library copy: " << (bytes / 1048576) << " MB\n";

    auto ids = firstTrackIds((onRam / "Engine Library").string(), items);
    std::cout << "tracks written per run: " << ids.size() << "\n\n";
    if (ids.empty()) {
        std::cerr << "no tracks in this Engine Library\n";
        return 1;
    }

    std::cout << "== One Engine cue write ==\n";
    const int n = static_cast<int>(ids.size());
    int failedStick = 0, failedRam = 0, failedHeld = 0;
    row("1. today, reopen per item, on the stick",
        timeReopenPerItem((onStick / "Engine Library").string(), ids, failedStick), n);
    row("2. today, reopen per item, on a ramdisk",
        timeReopenPerItem((onRam / "Engine Library").string(), ids, failedRam), n);
    row("3. proposed, one handle + one update",
        timeHeldHandle((onRam / "Engine Library").string(), ids, failedHeld), n);
    std::cout << "\n  tracks the write refused: " << failedStick << " reopening, " << failedHeld
              << " via snapshot+update (of " << n << ")\n";
    if (failedHeld > failedStick) {
        std::cout << "  NOTE: snapshot()/update() refuses tracks the current writer handles. That is a\n"
                     "        correctness blocker for that shape on this library, not a timing detail.\n";
    }

    fs::remove_all(onStick);
    fs::remove_all(onRam);
    std::cout << "\nScratch removed. The real Engine Library was only read.\n";
    return 0;
}
