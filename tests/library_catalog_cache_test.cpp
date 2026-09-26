// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#include "gui/library_catalog_cache.hpp"

using namespace seabass::gui;
using namespace std::chrono_literals;
using seabass::application::CancellationToken;
using seabass::application::OperationCancelled;

namespace
{

std::vector<seabass::domain::Track> oneTrack(const std::string &sourceId)
{
    seabass::domain::Track t;
    t.sourceId = sourceId;
    return {t};
}


// A door a fake pass stands at until the test opens it, and a note that
// it got there: the test decides the order of events, never a sleep.
class Gate
{
public:
    void open()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_open = true;
        }
        m_cv.notify_all();
    }
    // Called by the fake pass: says it has arrived, then waits.
    void arriveAndWait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_arrived = true;
        m_cv.notify_all();
        m_cv.wait(lock, [&] { return m_open; });
    }
    void waitUntilArrived()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&] { return m_arrived; });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_open = false;
    bool m_arrived = false;
};

using Detail = LibraryCatalogCache::Detail;

// A fake staged reader: records every pass it runs, fills each stage with
// something the test can tell apart (Tracks: two tracks named after the
// path; Cues: one cue each; Full: a size), and can hold any pass of any
// path at a gate.
struct FakeReader
{
    std::mutex mutex;
    std::vector<std::pair<Detail, std::string>> passes;
    std::map<std::pair<Detail, std::string>, Gate *> gates;
    // Set to make the next Cues pass of a path half-fill its copy and
    // then throw, the way a reader does when the stick is pulled.
    std::set<std::string> cuesThrowOnce;

    int count(Detail stage, const std::string &path)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<int>(std::count(passes.begin(), passes.end(), std::make_pair(stage, path)));
    }
    int countPath(const std::string &path)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<int>(std::count_if(passes.begin(), passes.end(),
                                              [&](const auto &pass) { return pass.second == path; }));
    }

    LibraryCatalogCache::StageFn stageFn()
    {
        return [this](Detail stage, const std::string &, const std::string &path,
                      std::vector<seabass::domain::Track> &tracks, seabass::application::ProgressReporter &,
                      CancellationToken cancel) {
            Gate *gate = nullptr;
            bool throwNow = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                passes.emplace_back(stage, path);
                const auto it = gates.find({stage, path});
                if (it != gates.end()) {
                    gate = it->second;
                }
                if (stage == Detail::Cues && cuesThrowOnce.erase(path) > 0) {
                    throwNow = true;
                }
            }
            if (gate) {
                gate->arriveAndWait();
            }
            switch (stage) {
            case Detail::Tracks:
                assert(tracks.empty());
                tracks = oneTrack(path);
                tracks.push_back(oneTrack(path + "#2").front());
                break;
            case Detail::Cues:
                assert(tracks.size() == 2 && tracks[0].cues.empty());
                for (auto &track : tracks) {
                    // Per track, then a check, like a real reader.
                    track.cues.push_back(seabass::domain::CuePoint{});
                    if (throwNow) {
                        throw std::runtime_error("stick pulled mid pass");
                    }
                    cancel.throwIfCancelled();
                }
                break;
            case Detail::Full:
                assert(tracks.size() == 2 && tracks[0].cues.size() == 1);
                for (auto &track : tracks) {
                    track.fileSizeBytes = 42;
                }
                break;
            }
        };
    }
};

LibraryCatalogCache::MtimeFn fixedMtime()
{
    return [](const std::string &, const std::string &) { return std::chrono::system_clock::time_point{}; };
}

bool readyNow(std::future<std::vector<seabass::domain::Track>> &future)
{
    return future.wait_for(0s) == std::future_status::ready;
}

void stagedCases()
{
    const std::string stick = "/stick/PIONEER";

    // Stage 1: each Detail reads only the passes it is missing, in
    // order, and each result carries exactly its stage.
    {
        FakeReader reader;
        LibraryCatalogCache cache(reader.stageFn(), fixedMtime());

        auto tracks = cache.tracksFor("rekordbox", stick, Detail::Tracks);
        assert(tracks.size() == 2 && tracks[0].cues.empty() && tracks[0].fileSizeBytes == 0);
        auto cues = cache.tracksFor("rekordbox", stick, Detail::Cues);
        assert(cues.size() == 2 && cues[0].cues.size() == 1 && cues[0].fileSizeBytes == 0);
        auto full = cache.tracksFor("rekordbox", stick, Detail::Full);
        assert(full.size() == 2 && full[0].cues.size() == 1 && full[0].fileSizeBytes == 42);
        const std::vector<std::pair<Detail, std::string>> inOrder{
            {Detail::Tracks, stick}, {Detail::Cues, stick}, {Detail::Full, stick}};
        assert(reader.passes == inOrder);

        // An earlier stage of a Full entry is a hit and holds everything.
        auto again = cache.tracksFor("rekordbox", stick, Detail::Tracks);
        assert(again[0].fileSizeBytes == 42);
        // The overload without a Detail is Full: a hit now.
        auto plain = cache.tracksFor("rekordbox", stick);
        assert(plain[0].fileSizeBytes == 42 && reader.passes.size() == 3);

        // Cold, a Full request runs all three passes itself, in order.
        FakeReader cold;
        LibraryCatalogCache coldCache(cold.stageFn(), fixedMtime());
        auto coldFull = coldCache.tracksFor("rekordbox", stick);
        assert(coldFull[0].fileSizeBytes == 42 && cold.passes == inOrder);
        std::cout << "stage 1 (stages read in order, each only what it lacks; no Detail means Full) OK\n";
    }

    // Stage 2: a Tracks request while the Cues pass is running returns at
    // once. The Cues pass is held at a gate that only opens AFTER the
    // Tracks request has returned, so a Tracks request that waited for
    // the pass could never return at all.
    {
        FakeReader reader;
        Gate cuesGate;
        reader.gates[{Detail::Cues, stick}] = &cuesGate;
        LibraryCatalogCache cache(reader.stageFn(), fixedMtime());

        auto cuesCall = std::async(std::launch::async, [&] { return cache.tracksFor("rekordbox", stick, Detail::Cues); });
        cuesGate.waitUntilArrived();

        const auto started = std::chrono::steady_clock::now();
        auto tracksCall = std::async(std::launch::async, [&] { return cache.tracksFor("rekordbox", stick, Detail::Tracks); });
        const bool returned = tracksCall.wait_for(10s) == std::future_status::ready;
        const auto took = std::chrono::steady_clock::now() - started;
        const bool cuesStillHeld = !readyNow(cuesCall);
        cuesGate.open();  // before asserting, so a failure cannot hang the process
        assert(returned && "a Tracks request must not wait for the Cues pass");
        assert(cuesStillHeld);
        auto tracks = tracksCall.get();
        assert(tracks.size() == 2 && tracks[0].cues.empty());
        assert(reader.count(Detail::Tracks, stick) == 1);
        auto cues = cuesCall.get();
        assert(cues[0].cues.size() == 1 && reader.count(Detail::Cues, stick) == 1);
        std::cout << "stage 2 (Tracks during the Cues pass returns at once, took "
                  << std::chrono::duration_cast<std::chrono::microseconds>(took).count() << " us) OK\n";
    }

    // Stage 3: a Cues request during the Cues pass waits for that pass
    // and gets its result; the pass runs once. The waiter is seen
    // waiting (waitingCallers), not assumed to be.
    {
        FakeReader reader;
        Gate cuesGate;
        reader.gates[{Detail::Cues, stick}] = &cuesGate;
        LibraryCatalogCache cache(reader.stageFn(), fixedMtime());

        auto first = std::async(std::launch::async, [&] { return cache.tracksFor("rekordbox", stick, Detail::Cues); });
        cuesGate.waitUntilArrived();
        auto second = std::async(std::launch::async, [&] { return cache.tracksFor("rekordbox", stick, Detail::Cues); });
        while (cache.waitingCallers() != 1) {
            std::this_thread::yield();
        }
        assert(!readyNow(second) && !readyNow(first));
        cuesGate.open();
        auto a = first.get();
        auto b = second.get();
        assert(a.size() == 2 && b.size() == 2 && a[0].cues.size() == 1 && b[0].cues.size() == 1);
        assert(a[0].sourceId == b[0].sourceId);
        assert(reader.count(Detail::Tracks, stick) == 1);
        assert(reader.count(Detail::Cues, stick) == 1 && "the waiter must not read the cues a second time");
        assert(reader.count(Detail::Full, stick) == 0);
        std::cout << "stage 3 (Cues during the Cues pass waits, one read) OK\n";
    }

    // Stage 4: a Full request during the Cues pass waits for it, then the
    // Full pass runs once, after it.
    {
        FakeReader reader;
        Gate cuesGate;
        reader.gates[{Detail::Cues, stick}] = &cuesGate;
        LibraryCatalogCache cache(reader.stageFn(), fixedMtime());

        auto cuesCall = std::async(std::launch::async, [&] { return cache.tracksFor("rekordbox", stick, Detail::Cues); });
        cuesGate.waitUntilArrived();
        auto fullCall = std::async(std::launch::async, [&] { return cache.tracksFor("rekordbox", stick, Detail::Full); });
        while (cache.waitingCallers() != 1) {
            std::this_thread::yield();
        }
        assert(reader.count(Detail::Full, stick) == 0);
        cuesGate.open();
        auto full = fullCall.get();
        cuesCall.get();
        assert(full[0].fileSizeBytes == 42);
        const std::vector<std::pair<Detail, std::string>> inOrder{
            {Detail::Tracks, stick}, {Detail::Cues, stick}, {Detail::Full, stick}};
        assert(reader.passes == inOrder);
        std::cout << "stage 4 (Full during the Cues pass runs after it, each pass once) OK\n";
    }

    // Stage 5: prefetch reads to Full in the background; a Full request
    // afterwards reads nothing, and prefetching it again reads nothing.
    {
        FakeReader reader;
        LibraryCatalogCache cache(reader.stageFn(), fixedMtime());
        cache.prefetch("rekordbox", stick);
        cache.waitUntilPrefetchIdle();
        const std::vector<std::pair<Detail, std::string>> inOrder{
            {Detail::Tracks, stick}, {Detail::Cues, stick}, {Detail::Full, stick}};
        assert(reader.passes == inOrder);
        auto full = cache.tracksFor("rekordbox", stick);
        assert(full[0].fileSizeBytes == 42 && reader.passes.size() == 3);
        cache.prefetch("rekordbox", stick);
        cache.waitUntilPrefetchIdle();
        assert(reader.passes.size() == 3);
        std::cout << "stage 5 (prefetch reaches Full, a later Full request reads nothing) OK\n";
    }

    // Stage 6: a foreground request for the stage the prefetch is
    // reading waits for the prefetch; one for an earlier stage does not.
    {
        FakeReader reader;
        Gate cuesGate;
        reader.gates[{Detail::Cues, stick}] = &cuesGate;
        LibraryCatalogCache cache(reader.stageFn(), fixedMtime());
        cache.prefetch("rekordbox", stick);
        cuesGate.waitUntilArrived();

        auto tracks = cache.tracksFor("rekordbox", stick, Detail::Tracks);
        assert(tracks.size() == 2 && tracks[0].cues.empty());
        auto cuesCall = std::async(std::launch::async, [&] { return cache.tracksFor("rekordbox", stick, Detail::Cues); });
        while (cache.waitingCallers() != 1) {
            std::this_thread::yield();
        }
        assert(!readyNow(cuesCall));
        cuesGate.open();
        auto cues = cuesCall.get();
        assert(cues[0].cues.size() == 1);
        cache.waitUntilPrefetchIdle();
        assert(reader.count(Detail::Tracks, stick) == 1);
        assert(reader.count(Detail::Cues, stick) == 1 && "the foreground must not re-read what the prefetch reads");
        assert(reader.count(Detail::Full, stick) == 1);
        std::cout << "stage 6 (a foreground request waits for the prefetch's pass, one read) OK\n";
    }

    // Stage 7: invalidateEveryCatalogOn() mid prefetch drops the queued
    // work for that stick, cancels the pass in flight so it caches
    // nothing, and leaves another stick's queued work alone.
    {
        FakeReader reader;
        Gate cuesGate;
        const std::string s1Pioneer = "/s1/PIONEER";
        const std::string s1Engine = "/s1/Engine Library";
        const std::string s2Pioneer = "/s2/PIONEER";
        reader.gates[{Detail::Cues, s1Pioneer}] = &cuesGate;
        LibraryCatalogCache cache(reader.stageFn(), fixedMtime());
        cache.prefetch("rekordbox", s1Pioneer);
        cache.prefetch("engine", s1Engine);
        cache.prefetch("rekordbox", s2Pioneer);
        cuesGate.waitUntilArrived();
        const auto before = cache.invalidationCount("rekordbox", s1Pioneer);

        cache.invalidateEveryCatalogOn("/s1");
        // The pass in flight finds its token cancelled at its next track.
        cuesGate.open();
        cache.waitUntilPrefetchIdle();

        assert(cache.invalidationCount("rekordbox", s1Pioneer) == before + 1);
        assert(cache.invalidationCount("onelibrary", s1Pioneer) >= 1);
        assert(cache.invalidationCount("engine", s1Engine) >= 1);
        assert(reader.countPath(s1Engine) == 0 && "queued work for the invalidated stick must be dropped");
        assert(reader.count(Detail::Full, s1Pioneer) == 0 && "a cancelled prefetch must not go on");
        assert(reader.count(Detail::Full, s2Pioneer) == 1 && "another stick's queue must be untouched");
        // Nothing of s1 is cached: not the cancelled Cues pass, and not
        // the Tracks stage read before the invalidation either.
        const int tracksBefore = reader.count(Detail::Tracks, s1Pioneer);
        auto again = cache.tracksFor("rekordbox", s1Pioneer, Detail::Cues);
        assert(again[0].cues.size() == 1);
        assert(reader.count(Detail::Tracks, s1Pioneer) == tracksBefore + 1);
        // s2 was prefetched to Full and still is.
        const auto s2Passes = reader.countPath(s2Pioneer);
        cache.tracksFor("rekordbox", s2Pioneer);
        assert(reader.countPath(s2Pioneer) == s2Passes);
        std::cout << "stage 7 (invalidation mid prefetch drops the stick's queue and its pass) OK\n";
    }

    // Stage 8: a pass that throws halfway (the stick pulled) leaves the
    // previous stage exactly as it was: never a truncated one. In the
    // background the worker survives it and goes on to the next catalog.
    {
        FakeReader reader;
        reader.cuesThrowOnce.insert(stick);
        LibraryCatalogCache cache(reader.stageFn(), fixedMtime());

        bool threw = false;
        try {
            cache.tracksFor("rekordbox", stick, Detail::Cues);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        assert(threw);
        auto tracks = cache.tracksFor("rekordbox", stick, Detail::Tracks);
        assert(tracks.size() == 2 && tracks[0].cues.empty() && "the half-filled Cues copy must not leak in");
        assert(reader.count(Detail::Tracks, stick) == 1);
        auto cues = cache.tracksFor("rekordbox", stick, Detail::Cues);
        assert(cues[0].cues.size() == 1 && cues[1].cues.size() == 1);
        assert(reader.count(Detail::Cues, stick) == 2);

        FakeReader background;
        const std::string other = "/other/PIONEER";
        background.cuesThrowOnce.insert(stick);
        LibraryCatalogCache bgCache(background.stageFn(), fixedMtime());
        bgCache.prefetch("rekordbox", stick);
        bgCache.prefetch("rekordbox", other);
        bgCache.waitUntilPrefetchIdle();
        assert(background.count(Detail::Full, stick) == 0);
        assert(background.count(Detail::Full, other) == 1);
        auto bgTracks = bgCache.tracksFor("rekordbox", stick, Detail::Tracks);
        assert(bgTracks[0].cues.empty() && background.count(Detail::Tracks, stick) == 1);
        std::cout << "stage 8 (a pass that throws caches nothing for its stage) OK\n";
    }

    // Stage 9: a catalog file that changed since the read starts over at
    // Tracks, whatever stage the entry had reached.
    {
        FakeReader reader;
        std::atomic<int> seconds{0};
        auto mtimeFn = [&](const std::string &, const std::string &) {
            return std::chrono::system_clock::time_point{} + std::chrono::seconds(seconds.load());
        };
        LibraryCatalogCache cache(reader.stageFn(), mtimeFn);
        cache.tracksFor("rekordbox", stick, Detail::Cues);
        seconds = 1;
        cache.tracksFor("rekordbox", stick, Detail::Tracks);
        assert(reader.count(Detail::Tracks, stick) == 2);
        assert(reader.count(Detail::Cues, stick) == 1);
        cache.tracksFor("rekordbox", stick, Detail::Cues);
        assert(reader.count(Detail::Cues, stick) == 2);
        std::cout << "stage 9 (a changed catalog file starts over at Tracks) OK\n";
    }

    // Stage 10: an invalidation while a waiter sits behind a pass frees
    // the waiter to read for itself, and the superseded pass commits
    // nothing.
    {
        FakeReader reader;
        Gate cuesGate;
        reader.gates[{Detail::Cues, stick}] = &cuesGate;
        LibraryCatalogCache cache(reader.stageFn(), fixedMtime());
        auto first = std::async(std::launch::async, [&] { return cache.tracksFor("rekordbox", stick, Detail::Cues); });
        cuesGate.waitUntilArrived();
        {
            std::lock_guard<std::mutex> lock(reader.mutex);
            reader.gates.clear();  // the waiter's own read must not stop at the gate
        }
        auto second = std::async(std::launch::async, [&] { return cache.tracksFor("rekordbox", stick, Detail::Cues); });
        while (cache.waitingCallers() != 1 && !readyNow(second)) {
            std::this_thread::yield();
        }
        cache.invalidate("rekordbox", stick);
        auto b = second.get();  // returns while the first pass is still held
        assert(b[0].cues.size() == 1);
        assert(!readyNow(first));
        cuesGate.open();
        first.get();
        const int passes = static_cast<int>(reader.passes.size());
        cache.tracksFor("rekordbox", stick, Detail::Cues);
        assert(static_cast<int>(reader.passes.size()) == passes && "the waiter's read is the cached one");
        std::cout << "stage 10 (an invalidation frees the waiters; the superseded pass commits nothing) OK\n";
    }

    // Stage 11: a waiter's own token ends its wait. The Cues pass is held
    // at a gate; a second Cues request is seen waiting, its token is
    // cancelled, and it must leave with OperationCancelled while the
    // gate is still shut. The pass still runs exactly once.
    {
        FakeReader reader;
        Gate cuesGate;
        reader.gates[{Detail::Cues, stick}] = &cuesGate;
        LibraryCatalogCache cache(reader.stageFn(), fixedMtime());

        auto first = std::async(std::launch::async, [&] { return cache.tracksFor("rekordbox", stick, Detail::Cues); });
        cuesGate.waitUntilArrived();
        CancellationToken token;
        auto waiter = std::async(std::launch::async, [&] {
            try {
                cache.tracksFor("rekordbox", stick, Detail::Cues, seabass::application::NullProgressReporter::instance(),
                                token);
                return std::string("returned");
            } catch (const seabass::application::OperationCancelled &) {
                return std::string("cancelled");
            }
        });
        while (cache.waitingCallers() == 0) {
            std::this_thread::yield();
        }
        token.cancel();
        const bool left = waiter.wait_for(5s) == std::future_status::ready;
        const bool cuesStillHeld = !readyNow(first);
        cuesGate.open();  // before asserting, so a failure cannot hang the process
        assert(left && "a cancelled waiter must leave while the pass is still held");
        assert(cuesStillHeld);
        assert(waiter.get() == "cancelled");
        first.get();
        assert(reader.count(Detail::Cues, stick) == 1);
        std::cout << "stage 11 (a cancelled waiter leaves the wait) OK\n";
    }
}

}  // namespace

int main()
{
    // Case 1: a second call for the same (format, path), unchanged mtime,
    // is served from the cache -- the scan function runs exactly once.
    {
        std::atomic<int> scanCount{0};
        auto scanFn = [&](const std::string &, const std::string &, seabass::application::ProgressReporter &, CancellationToken) {
            scanCount++;
            return oneTrack("1");
        };
        auto mtimeFn = [](const std::string &, const std::string &) { return std::chrono::system_clock::time_point{}; };
        LibraryCatalogCache cache(scanFn, mtimeFn);

        auto a = cache.tracksFor("rekordbox", "/stick");
        auto b = cache.tracksFor("rekordbox", "/stick");
        assert(scanCount == 1);
        assert(a.size() == 1 && b.size() == 1);
        assert(a[0].sourceId == "1" && b[0].sourceId == "1");
        std::cout << "case 1 (unchanged mtime -> cache hit, scans once) OK\n";
    }

    // Case 2: a changed mtime forces a re-scan.
    {
        std::atomic<int> scanCount{0};
        auto scanFn = [&](const std::string &, const std::string &, seabass::application::ProgressReporter &, CancellationToken) {
            scanCount++;
            return oneTrack("1");
        };
        std::chrono::system_clock::time_point mtime{};
        auto mtimeFn = [&](const std::string &, const std::string &) { return mtime; };
        LibraryCatalogCache cache(scanFn, mtimeFn);

        cache.tracksFor("rekordbox", "/stick");
        assert(scanCount == 1);
        mtime += 1s;
        cache.tracksFor("rekordbox", "/stick");
        assert(scanCount == 2);
        std::cout << "case 2 (changed mtime -> re-scan) OK\n";
    }

    // Case 3: invalidate() forces a re-scan even though mtime hasn't moved.
    {
        std::atomic<int> scanCount{0};
        auto scanFn = [&](const std::string &, const std::string &, seabass::application::ProgressReporter &, CancellationToken) {
            scanCount++;
            return oneTrack("1");
        };
        auto mtimeFn = [](const std::string &, const std::string &) { return std::chrono::system_clock::time_point{}; };
        LibraryCatalogCache cache(scanFn, mtimeFn);

        cache.tracksFor("rekordbox", "/stick");
        cache.tracksFor("rekordbox", "/stick");
        assert(scanCount == 1);
        cache.invalidate("rekordbox", "/stick");
        cache.tracksFor("rekordbox", "/stick");
        assert(scanCount == 2);
        std::cout << "case 3 (invalidate() forces re-scan) OK\n";
    }

    // Case 4: two threads requesting the same uncached key at roughly the
    // same time only trigger one real scan -- the second waits for the
    // first instead of racing it. The scan function sleeps to widen the
    // window in which the race would show up if the "in progress" gating
    // didn't work.
    {
        std::atomic<int> scanCount{0};
        auto scanFn = [&](const std::string &, const std::string &, seabass::application::ProgressReporter &, CancellationToken) {
            scanCount++;
            std::this_thread::sleep_for(100ms);
            return oneTrack("1");
        };
        auto mtimeFn = [](const std::string &, const std::string &) { return std::chrono::system_clock::time_point{}; };
        LibraryCatalogCache cache(scanFn, mtimeFn);

        std::atomic<bool> go{false};
        auto worker = [&] {
            while (!go.load()) {
                std::this_thread::yield();
            }
            return cache.tracksFor("rekordbox", "/stick");
        };

        std::vector<seabass::domain::Track> resultA, resultB;
        std::thread t1([&] { resultA = worker(); });
        std::thread t2([&] { resultB = worker(); });
        go = true;
        t1.join();
        t2.join();

        assert(scanCount == 1);
        assert(resultA.size() == 1 && resultB.size() == 1);
        std::cout << "case 4 (concurrent callers for the same key scan once) OK\n";
    }

    // Case 5: different keys (different format, or different path) are
    // cached independently.
    {
        std::atomic<int> scanCount{0};
        auto scanFn = [&](const std::string &format, const std::string &, seabass::application::ProgressReporter &, CancellationToken) {
            scanCount++;
            return oneTrack(format);
        };
        auto mtimeFn = [](const std::string &, const std::string &) { return std::chrono::system_clock::time_point{}; };
        LibraryCatalogCache cache(scanFn, mtimeFn);

        auto rb = cache.tracksFor("rekordbox", "/stick");
        auto engine = cache.tracksFor("engine", "/stick");
        assert(scanCount == 2);
        assert(rb[0].sourceId == "rekordbox" && engine[0].sourceId == "engine");
        std::cout << "case 5 (different keys cached independently) OK\n";
    }

    // Case 6: invalidate() landing while a scan is already in flight for
    // that key must not be silently undone once that scan completes.
    {
        std::atomic<int> scanCount{0};
        std::atomic<bool> firstScanStarted{false};
        std::atomic<bool> proceedWithFirstScan{false};
        auto scanFn = [&](const std::string &, const std::string &, seabass::application::ProgressReporter &, CancellationToken) {
            int n = ++scanCount;
            if (n == 1) {
                firstScanStarted = true;
                while (!proceedWithFirstScan.load()) {
                    std::this_thread::yield();
                }
            }
            return oneTrack("1");
        };
        auto mtimeFn = [](const std::string &, const std::string &) { return std::chrono::system_clock::time_point{}; };
        LibraryCatalogCache cache(scanFn, mtimeFn);

        std::thread t1([&] { cache.tracksFor("rekordbox", "/stick"); });
        while (!firstScanStarted.load()) {
            std::this_thread::yield();
        }
        // Invalidate while the first scan is still blocked mid-flight,
        // then let it finish.
        cache.invalidate("rekordbox", "/stick");
        proceedWithFirstScan = true;
        t1.join();

        assert(scanCount == 1);
        // A fresh call must re-scan -- the in-flight scan's result must
        // not have been cached despite completing after the invalidate().
        cache.tracksFor("rekordbox", "/stick");
        assert(scanCount == 2);
        std::cout << "case 6 (invalidate() during an in-flight scan isn't undone by its completion) OK\n";
    }

    // Case 7: invalidateWithOneLibraryMirror() also invalidates
    // "onelibrary" at the same path when format is "rekordbox" (the
    // rekordbox-primary-write-mirrors-into-OneLibrary shape several
    // controllers share), but leaves "onelibrary" alone for any other
    // format -- it's a mirror of rekordbox specifically, not a blanket
    // "also invalidate onelibrary" for every write.
    {
        std::atomic<int> scanCount{0};
        auto scanFn = [&](const std::string &, const std::string &, seabass::application::ProgressReporter &, CancellationToken) {
            scanCount++;
            return oneTrack("1");
        };
        auto mtimeFn = [](const std::string &, const std::string &) { return std::chrono::system_clock::time_point{}; };
        LibraryCatalogCache cache(scanFn, mtimeFn);

        cache.tracksFor("rekordbox", "/stick");
        cache.tracksFor("onelibrary", "/stick");
        cache.tracksFor("engine", "/stick");
        assert(scanCount == 3);

        cache.invalidateWithOneLibraryMirror("rekordbox", "/stick");
        cache.tracksFor("rekordbox", "/stick");
        cache.tracksFor("onelibrary", "/stick");
        assert(scanCount == 5);  // both rekordbox and its onelibrary mirror re-scanned

        cache.invalidateWithOneLibraryMirror("engine", "/stick");
        cache.tracksFor("engine", "/stick");
        cache.tracksFor("onelibrary", "/stick");
        assert(scanCount == 6);  // engine re-scanned, onelibrary still cached (not a mirror of engine)
        std::cout << "case 7 (invalidateWithOneLibraryMirror only mirrors rekordbox) OK\n";
    }

    // Case 8: a cancelled scan throws OperationCancelled, caches nothing
    // (the next call scans from scratch), and a concurrent waiter on the
    // same key is not handed the cancelled result -- it scans for itself.
    {
        std::atomic<int> scanCount{0};
        std::atomic<bool> firstScanStarted{false};
        auto scanFn = [&](const std::string &, const std::string &, seabass::application::ProgressReporter &,
                          CancellationToken cancel) {
            int n = ++scanCount;
            if (n == 1) {
                firstScanStarted = true;
                while (!cancel.cancelled()) {
                    std::this_thread::yield();
                }
                cancel.throwIfCancelled();
            }
            return oneTrack("complete");
        };
        auto mtimeFn = [](const std::string &, const std::string &) { return std::chrono::system_clock::time_point{}; };
        LibraryCatalogCache cache(scanFn, mtimeFn);

        CancellationToken token;
        bool cancelledSeen = false;
        std::thread t1([&] {
            try {
                cache.tracksFor("rekordbox", "/stick", seabass::application::NullProgressReporter::instance(), token);
            } catch (const OperationCancelled &) {
                cancelledSeen = true;
            }
        });
        while (!firstScanStarted.load()) {
            std::this_thread::yield();
        }
        // A second caller arrives while the first is in flight and waits.
        std::vector<seabass::domain::Track> second;
        std::thread t2([&] { second = cache.tracksFor("rekordbox", "/stick"); });
        std::this_thread::sleep_for(50ms);
        token.cancel();
        t1.join();
        t2.join();

        assert(cancelledSeen);
        assert(scanCount == 2);  // the waiter scanned for itself
        assert(second.size() == 1 && second[0].sourceId == "complete");
        // Cached now, from the complete scan only.
        cache.tracksFor("rekordbox", "/stick");
        assert(scanCount == 2);

        // An already-cancelled token never even starts the scan.
        LibraryCatalogCache fresh(scanFn, mtimeFn);
        CancellationToken dead;
        dead.cancel();
        bool threw = false;
        try {
            fresh.tracksFor("engine", "/stick", seabass::application::NullProgressReporter::instance(), dead);
        } catch (const OperationCancelled &) {
            threw = true;
        }
        assert(threw && scanCount == 2);
        std::cout << "case 8 (a cancelled scan caches nothing and frees its waiters) OK\n";
    }

    // Case 7b: one entry for every spelling of a catalog path. A page hands
    // the native form on Windows, a session derives its sibling with a
    // slash, and a trailing separator is the same place: all of them hit
    // the one cache line, and invalidating under any spelling clears it.
    {
        std::atomic<int> scanCount{0};
        auto scanFn = [&](const std::string &, const std::string &, seabass::application::ProgressReporter &, CancellationToken) {
            scanCount++;
            return oneTrack("1");
        };
        auto mtimeFn = [](const std::string &, const std::string &) { return std::chrono::system_clock::time_point{}; };
        LibraryCatalogCache cache(scanFn, mtimeFn);

        cache.tracksFor("rekordbox", "/stick/PIONEER");
        cache.tracksFor("rekordbox", "/stick/PIONEER/");
        cache.tracksFor("rekordbox", "/stick//PIONEER");
        cache.tracksFor("rekordbox", "\\stick\\PIONEER");
        assert(scanCount == 1);
        cache.invalidate("rekordbox", "/stick/PIONEER/");
        cache.tracksFor("rekordbox", "/stick/PIONEER");
        assert(scanCount == 2);
        std::cout << "case 7b (one entry per catalog, whatever the spelling) OK\n";
    }

    stagedCases();

    std::cout << "All library_catalog_cache tests passed.\n";
    return 0;
}
