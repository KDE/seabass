// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// How many tracks are analysed at once, and when the next may start:
//
// 1. Workers by cores: one a performance core, one for two efficiency
//    cores, two at least, eight at most; half the threads when the cores
//    are not known.
// 2. This computer answers: at least one core, and free memory.
// 3. The next track may start when none is in hand, whatever is free;
//    otherwise only with room for what is in hand, a typical track and
//    the margin. Unknown memory is taken for 4 GB.
// 4. The gate: a second track waits while memory is short and starts when
//    the first leaves; it starts at once when memory is there; a long
//    track in hand counts for its length; a cancel ends the wait.

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

#include "gui/analysis_resources.hpp"

using namespace seabass;
using namespace seabass::gui;

int main()
{
    const std::uint64_t GB = 1024ull * 1024 * 1024;
    {
        assert(analysisJobsFor({4, 4}, 8) == 6);
        assert(analysisJobsFor({8, 4}, 12) == 8);
        assert(analysisJobsFor({8, 0}, 16) == 8);
        assert(analysisJobsFor({2, 0}, 4) == 2);
        assert(analysisJobsFor({1, 0}, 2) == 2);
        assert(analysisJobsFor({2, 8}, 12) == 6);
        assert(analysisJobsFor({16, 0}, 32) == 8);
        assert(analysisJobsFor({0, 0}, 12) == 6);
        assert(analysisJobsFor({0, 0}, 1) == 2);
        std::cout << "case 1 (workers by cores) OK\n";
    }
    {
        const CpuCores cores = cpuCores();
        assert(cores.performance >= 1);
        assert(cores.efficiency >= 0);
        const auto free = availableMemoryBytes();
        assert(free && *free > 16 * 1024 * 1024);
        std::cout << "case 2 (this computer: " << cores.performance << " + " << cores.efficiency << " cores, "
                  << (*free >> 20) << " MB free) OK\n";
    }
    {
        assert(analysisMayStart(0, 0, 0));
        assert(analysisMayStart(std::nullopt, 100 * GB, 0));
        const std::uint64_t one = AnalysisTypicalBytes + AnalysisMemoryMargin;
        assert(analysisMayStart(one, 0, 1));
        assert(!analysisMayStart(one - 1, 0, 1));
        assert(!analysisMayStart(one, 1, 1));
        assert(analysisMayStart(one + 2 * GB, 2 * GB, 3));
        assert(!analysisMayStart(one + 2 * GB - 1, 2 * GB, 3));
        assert(analysisMayStart(std::nullopt, AnalysisMemoryWhenUnknown - one, 1));
        assert(!analysisMayStart(std::nullopt, AnalysisMemoryWhenUnknown - one + 1, 1));
        std::cout << "case 3 (when the next track may start) OK\n";
    }
    {
        std::atomic<std::uint64_t> free{AnalysisTypicalBytes + AnalysisMemoryMargin};
        AnalysisMemoryGate gate([&]() { return std::optional<std::uint64_t>(free.load()); });
        const application::CancellationToken cancel;
        const int first = gate.enter(cancel);
        assert(gate.inHand() == 1 && gate.stillToTake() == AnalysisTypicalBytes);
        // Short of the first track's own share: the second waits.
        std::atomic<int> second{0};
        std::thread waiting([&]() { second = gate.enter(cancel); });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        assert(second == 0);
        gate.leave(first);
        waiting.join();
        assert(second != 0 && gate.inHand() == 1);

        // Memory for both: the next starts at once.
        free = 2 * AnalysisTypicalBytes + AnalysisMemoryMargin;
        const int third = gate.enter(cancel);
        assert(gate.inHand() == 2);
        gate.leave(third);

        // The track in hand has read a minute: the rest of a typical
        // track's share counts against the next.
        const std::uint64_t held = analysisBytesFor(60.0, 44100);
        assert(held == AnalysisBaseBytes + AnalysisBytesPerMinute);
        gate.noteRead(second, 60.0, 44100);
        const std::uint64_t rest = AnalysisTypicalBytes - held;
        assert(gate.stillToTake() == rest);
        free = rest + AnalysisTypicalBytes + AnalysisMemoryMargin - 1;
        std::atomic<int> fourth{0};
        std::thread longWait([&]() { fourth = gate.enter(cancel); });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        assert(fourth == 0);
        // Memory that something else gave back is seen without a leave.
        free = rest + AnalysisTypicalBytes + AnalysisMemoryMargin;
        longWait.join();
        assert(fourth != 0);
        gate.leave(fourth);

        // An hour in, nothing is held back for it any more: what it takes
        // from there on shows in the free memory. An hour is 265 MB, and
        // a high sample rate 40 MB more.
        gate.noteRead(second, 3600.0, 44100);
        assert(gate.stillToTake() == 0);
        assert(analysisBytesFor(3600.0, 48000) == AnalysisBaseBytes + 60 * AnalysisBytesPerMinute);
        assert(analysisBytesFor(3600.0, 48000) < 270ull * 1024 * 1024);
        assert(analysisBytesFor(0.0, 96000) == AnalysisBaseBytes + AnalysisHighRateBytes);
        assert(analysisBytesFor(-5.0, 44100) == AnalysisBaseBytes);

        // A cancel ends a wait.
        free = 0;
        application::CancellationToken stop;
        std::atomic<bool> thrown{false};
        std::thread cancelled([&]() {
            try {
                gate.enter(stop);
            } catch (const application::OperationCancelled &) {
                thrown = true;
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        assert(!thrown);
        stop.cancel();
        cancelled.join();
        assert(thrown && gate.inHand() == 1);
        gate.leave(second);
        assert(gate.inHand() == 0 && gate.stillToTake() == 0);
        std::cout << "case 4 (the gate) OK\n";
    }
    std::cout << "analysis_resources_test: all cases passed\n";
    return 0;
}
