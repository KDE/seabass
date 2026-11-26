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
// 4. The gate at the start of a track: a second track waits while memory
//    is short and starts when the first leaves; it starts at once when
//    memory is there; a track in hand counts for what a typical track
//    still has to take, and nothing once it is past that length; a cancel
//    ends the wait.
// 5. The gate while tracks read: with free memory under the margin, a
//    younger track waits at its next piece and the oldest goes on (and is
//    not even asked about memory); the oldest leaving releases the
//    younger, and so does memory coming back; a cancel ends the wait;
//    memory that cannot be read never makes a track wait.
// 6. Free memory is read for a piece at most once a second (the gate
//    keeps its last reading; the clock is the test's), at every look
//    while a track waits, and never while the gate's lock is held.
//
// A wait is proved by the gate asking for free memory again (the Reader
// counts the questions), never by a sleep.

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "gui/analysis_resources.hpp"

using namespace seabass;
using namespace seabass::gui;

namespace
{

// Until `ready` holds, for ten seconds at most.
template<typename Ready>
bool within(Ready ready)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!ready()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

}  // namespace

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
        std::atomic<int> asked{0};
        AnalysisMemoryGate gate([&]() {
            ++asked;
            return std::optional<std::uint64_t>(free.load());
        });
        const application::CancellationToken cancel;
        const int first = gate.enter(cancel);
        assert(gate.inHand() == 1 && gate.stillToTake() == AnalysisTypicalBytes);
        // Short of the first track's own share: the second waits. Asked
        // twice, it looked, waited and looked again, and memory was short
        // both times.
        std::atomic<int> second{0};
        int askedBefore = asked;
        std::thread waiting([&]() { second = gate.enter(cancel); });
        assert(within([&] { return asked >= askedBefore + 2; }));
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
        gate.noteRead(second, 60.0, 44100, cancel);
        const std::uint64_t rest = AnalysisTypicalBytes - held;
        assert(gate.stillToTake() == rest);
        free = rest + AnalysisTypicalBytes + AnalysisMemoryMargin - 1;
        std::atomic<int> fourth{0};
        askedBefore = asked;
        std::thread longWait([&]() { fourth = gate.enter(cancel); });
        assert(within([&] { return asked >= askedBefore + 2; }));
        assert(fourth == 0);
        // Memory that something else gave back is seen without a leave.
        free = rest + AnalysisTypicalBytes + AnalysisMemoryMargin;
        longWait.join();
        assert(fourth != 0);
        gate.leave(fourth);

        // An hour in, nothing is held back for it any more: what it takes
        // from there on shows in the free memory. An hour is 265 MB, and
        // a high sample rate 40 MB more.
        gate.noteRead(second, 3600.0, 44100, cancel);
        assert(gate.stillToTake() == 0);
        assert(analysisBytesFor(3600.0, 48000) == AnalysisBaseBytes + 60 * AnalysisBytesPerMinute);
        assert(analysisBytesFor(3600.0, 48000) < 270ull * 1024 * 1024);
        assert(analysisBytesFor(0.0, 96000) == AnalysisBaseBytes + AnalysisHighRateBytes);
        assert(analysisBytesFor(-5.0, 44100) == AnalysisBaseBytes);

        // A cancel ends a wait.
        free = 0;
        application::CancellationToken stop;
        std::atomic<bool> thrown{false};
        askedBefore = asked;
        std::thread cancelled([&]() {
            try {
                gate.enter(stop);
            } catch (const application::OperationCancelled &) {
                thrown = true;
            }
        });
        assert(within([&] { return asked >= askedBefore + 2; }));
        assert(!thrown);
        stop.cancel();
        cancelled.join();
        assert(thrown && gate.inHand() == 1);
        gate.leave(second);
        assert(gate.inHand() == 0 && gate.stillToTake() == 0);
        std::cout << "case 4 (the gate at the start of a track) OK\n";
    }
    {
        std::atomic<std::uint64_t> free{100 * GB};
        std::atomic<bool> unknown{false};
        std::atomic<int> asked{0};
        // A clock two seconds on at every look: no reading is ever young
        // enough to go by, so each look asks (case 6 has the reading kept).
        std::atomic<long long> clockMs{0};
        AnalysisMemoryGate gate(
            [&]() {
                ++asked;
                return unknown ? std::nullopt : std::optional<std::uint64_t>(free.load());
            },
            [&]() { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(clockMs += 2000)); });
        const application::CancellationToken cancel;
        const int older = gate.enter(cancel);
        const int younger = gate.enter(cancel);
        const int youngest = gate.enter(cancel);
        assert(older < younger && younger < youngest);
        free = AnalysisMemoryMargin - 1;

        // The oldest goes on, and is not asked about memory at all.
        int askedBefore = asked;
        std::atomic<bool> done{false};
        std::thread oldest([&]() {
            gate.noteRead(older, 600.0, 44100, cancel);
            done = true;
        });
        assert(within([&] { return done.load(); }) && "the oldest in hand never waits");
        oldest.join();
        assert(asked == askedBefore);

        // A younger one waits at its piece; its read is noted all the same.
        done = false;
        askedBefore = asked;
        std::thread reading([&]() {
            gate.noteRead(younger, 600.0, 44100, cancel);
            done = true;
        });
        assert(within([&] { return asked >= askedBefore + 2; }));
        assert(!done);
        assert(gate.stillToTake() == AnalysisTypicalBytes);  // the youngest's share alone

        // Memory comes back: it goes on.
        free = AnalysisMemoryMargin;
        assert(within([&] { return done.load(); }));
        reading.join();

        // Short again: the youngest waits, the older leaving does not
        // release it (younger is in hand still), younger leaving does.
        free = AnalysisMemoryMargin - 1;
        done = false;
        askedBefore = asked;
        std::thread last([&]() {
            gate.noteRead(youngest, 60.0, 44100, cancel);
            done = true;
        });
        assert(within([&] { return asked >= askedBefore + 2; }));
        gate.leave(older);
        askedBefore = asked;
        assert(within([&] { return asked >= askedBefore + 2; }));
        assert(!done && "a track younger than another in hand still waits");
        gate.leave(younger);
        assert(within([&] { return done.load(); }) && "the oldest in hand never waits");
        last.join();

        // A cancel ends the wait.
        free = 100 * GB;
        const int another = gate.enter(cancel);
        assert(another > youngest);
        free = AnalysisMemoryMargin - 1;
        application::CancellationToken stop;
        std::atomic<bool> thrown{false};
        askedBefore = asked;
        std::thread cancelled([&]() {
            try {
                gate.noteRead(another, 60.0, 44100, stop);
            } catch (const application::OperationCancelled &) {
                thrown = true;
            }
        });
        assert(within([&] { return asked >= askedBefore + 2; }));
        assert(!thrown);
        stop.cancel();
        assert(within([&] { return thrown.load(); }) && "a cancel ends the wait");
        cancelled.join();

        // Memory that cannot be read: nobody waits.
        unknown = true;
        askedBefore = asked;
        done = false;
        std::thread blind([&]() {
            gate.noteRead(another, 120.0, 44100, cancel);
            done = true;
        });
        assert(within([&] { return done.load(); }) && "unknown memory never makes a track wait");
        blind.join();
        assert(asked == askedBefore + 1);
        gate.leave(another);
        gate.leave(youngest);
        assert(gate.inHand() == 0);
        std::cout << "case 5 (the gate while tracks read: the younger waits while memory is short, the oldest "
                     "never) OK\n";
    }
    {
        // The clock stands still unless the test moves it.
        std::atomic<long long> clockMs{0};
        std::atomic<std::uint64_t> free{100 * GB};
        std::atomic<int> asked{0};
        std::atomic<bool> readWithoutTheLock{true};
        std::vector<std::thread> lookers;
        std::mutex lookersMutex;
        AnalysisMemoryGate *gatePointer = nullptr;
        AnalysisMemoryGate gate(
            [&]() {
                ++asked;
                // Another thread asks the gate while it reads: it answers
                // only if the gate's lock is not held.
                auto answered = std::make_shared<std::atomic<bool>>(false);
                {
                    const std::lock_guard<std::mutex> lock(lookersMutex);
                    lookers.emplace_back([gatePointer, answered]() {
                        (void)gatePointer->inHand();
                        *answered = true;
                    });
                }
                if (!within([&] { return answered->load(); })) {
                    readWithoutTheLock = false;
                }
                return std::optional<std::uint64_t>(free.load());
            },
            [&]() { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(clockMs.load())); });
        gatePointer = &gate;
        const application::CancellationToken cancel;
        const int older = gate.enter(cancel);
        const int younger = gate.enter(cancel);
        assert(asked == 2 && "enter() reads every time");
        assert(readWithoutTheLock && "enter() reads free memory without the gate's lock");

        // Many pieces within a second: the reading enter() made is used.
        for (int piece = 1; piece <= 50; ++piece) {
            gate.noteRead(younger, piece * 1.5, 44100, cancel);
        }
        assert(asked == 2 && "pieces within a second of the last reading do not read again");
        clockMs = 999;
        gate.noteRead(younger, 80.0, 44100, cancel);
        assert(asked == 2);
        // A second on: one reading, and the pieces after it go by it.
        clockMs = 1000;
        for (int piece = 0; piece < 50; ++piece) {
            gate.noteRead(younger, 90.0 + piece, 44100, cancel);
        }
        assert(asked == 3 && "once a second at most");
        assert(readWithoutTheLock && "noteRead() reads free memory without the gate's lock");
        // The oldest is never asked about memory.
        clockMs = 5000;
        gate.noteRead(older, 100.0, 44100, cancel);
        assert(asked == 3);

        // Short of memory: a waiting track reads again at every look,
        // the clock standing still.
        free = AnalysisMemoryMargin - 1;
        std::atomic<bool> done{false};
        std::thread waiting([&]() {
            gate.noteRead(younger, 200.0, 44100, cancel);
            done = true;
        });
        assert(within([&] { return asked >= 3 + 3; }) && "a waiting track looks again at each tick");
        assert(!done);
        free = AnalysisMemoryMargin;
        assert(within([&] { return done.load(); }));
        waiting.join();
        gate.leave(younger);
        gate.leave(older);
        assert(gate.inHand() == 0);
        {
            const std::lock_guard<std::mutex> lock(lookersMutex);
            for (std::thread &looker : lookers) {
                looker.join();
            }
        }
        assert(readWithoutTheLock && "free memory is never read under the gate's lock");
        std::cout << "case 6 (free memory read once a second at most, at every look while a track waits, and "
                     "never under the lock) OK\n";
    }
    std::cout << "analysis_resources_test: all cases passed\n";
    return 0;
}
