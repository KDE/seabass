// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>

#include <filesystem>
#include <fstream>

#include "application/use_cases/fill_file_sizes.hpp"
#include "gui/library_catalog_cache.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "mp3_fixture.hpp"
#include "scratch_path.hpp"

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
    // What each Full pass found in the notes the Tracks pass left.
    std::vector<std::vector<std::string>> notesSeenByFull;

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
                      std::vector<seabass::domain::Track> &tracks, LibraryCatalogCache::StageNotes &notes,
                      seabass::application::ProgressReporter &, CancellationToken cancel) {
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
                assert(notes.unverifiedDurationPaths.empty() && "a Tracks pass starts from fresh notes");
                tracks = oneTrack(path);
                tracks.push_back(oneTrack(path + "#2").front());
                notes.unverifiedDurationPaths = {path + "/taken on trust"};
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
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    notesSeenByFull.push_back(notes.unverifiedDurationPaths);
                }
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

    // Stage 12: the stage comes back with the tracks, from the entry that
    // served them. An entry at Cues answering a Tracks request says Cues:
    // the fingerprint reader decides "cues known" from this, and asking
    // for the stage separately raced a cue pass committing in between.
    {
        FakeReader reader;
        LibraryCatalogCache cache(reader.stageFn(), fixedMtime());
        const auto cold = cache.stagedTracksFor("rekordbox", stick, Detail::Tracks);
        assert(cold.stage == Detail::Tracks && cold.tracks[0].cues.empty());
        cache.tracksFor("rekordbox", stick, Detail::Cues);
        const auto served = cache.stagedTracksFor("rekordbox", stick, Detail::Tracks);
        assert(served.stage == Detail::Cues && "an entry at Cues answers a Tracks request with stage Cues");
        assert(served.tracks[0].cues.size() == 1 && "and the tracks it hands out carry those cues");
        assert(reader.count(Detail::Cues, stick) == 1);
        const auto full = cache.stagedTracksFor("rekordbox", stick, Detail::Full);
        assert(full.stage == Detail::Full && full.tracks[0].fileSizeBytes == 42);
        assert(cache.stagedTracksFor("rekordbox", stick, Detail::Tracks).stage == Detail::Full);
        std::cout << "stage 12 (the stage reached comes back with the tracks, under one lock) OK\n";
    }

    // Stage 13: what the Tracks pass notes (the durations it took from the
    // cache unverified) reaches the Full pass of the same entry, and a
    // re-read after an invalidation starts from fresh notes.
    {
        FakeReader reader;
        LibraryCatalogCache cache(reader.stageFn(), fixedMtime());
        cache.tracksFor("rekordbox", stick, Detail::Tracks);
        cache.tracksFor("rekordbox", stick, Detail::Cues);
        cache.tracksFor("rekordbox", stick, Detail::Full);
        assert(reader.notesSeenByFull.size() == 1);
        assert(reader.notesSeenByFull[0] == std::vector<std::string>{stick + "/taken on trust"});
        cache.invalidate("rekordbox", stick);
        cache.tracksFor("rekordbox", stick);  // the fake asserts its Tracks pass gets empty notes
        assert(reader.notesSeenByFull.size() == 2);
        std::cout << "stage 13 (the Tracks pass's notes reach the Full pass) OK\n";
    }

    // Stage 14: a stick pulled in the middle of the Full pass. The pass is
    // the real application::fillFileSizes over real files; the stick is
    // "pulled" (invalidateEveryCatalogOn) from inside the pass, right
    // after its third stat, and the files' stats are counted as they run.
    // The prefetch worker must stop within one file: no stat after that
    // one, nothing cached at Full, and the worker idle again.
    {
        namespace fs = std::filesystem;
        const fs::path root = seabass::testing::scratchRoot() / "library_catalog_cache_test_full_cancel";
        fs::remove_all(root);
        const fs::path contents = root / "Contents";
        fs::create_directories(contents);
        constexpr int fileCount = 20;
        for (int i = 0; i < fileCount; ++i) {
            std::ofstream(contents / ("t" + std::to_string(i) + ".mp3"), std::ios::binary) << "audio";
        }
        const std::string pioneer = seabass::pathToUtf8(root / "PIONEER");
        const std::string stickRoot = seabass::pathToUtf8(root);

        struct PullingReporter : seabass::application::ProgressReporter
        {
            std::function<void()> pull;
            std::atomic<int> statted{0};
            std::atomic<int> afterPull{0};
            bool pulled = false;
            void start(const std::string &, size_t) override {}
            void finish() override {}
            void warn(const std::string &) override {}
            void tick(size_t) override
            {
                ++statted;
                if (pulled) {
                    ++afterPull;
                } else if (statted == 3) {
                    pulled = true;
                    pull();
                }
            }
        };
        PullingReporter reporter;
        std::atomic<int> fullPasses{0};
        std::atomic<int> fullCompleted{0};
        auto stage = [&](Detail detail, const std::string &, const std::string &, std::vector<seabass::domain::Track> &tracks,
                         LibraryCatalogCache::StageNotes &, seabass::application::ProgressReporter &,
                         CancellationToken cancel) {
            if (detail == Detail::Tracks) {
                for (int i = 0; i < fileCount; ++i) {
                    seabass::domain::Track track;
                    track.sourceId = std::to_string(i);
                    track.filePath = seabass::pathToUtf8(contents / ("t" + std::to_string(i) + ".mp3"));
                    tracks.push_back(track);
                }
            } else if (detail == Detail::Full) {
                ++fullPasses;
                seabass::application::fillFileSizes(tracks, cancel, reporter);
                ++fullCompleted;
            }
        };
        LibraryCatalogCache cache(stage, fixedMtime());
        reporter.pull = [&] { cache.invalidateEveryCatalogOn(stickRoot); };
        cache.prefetch("rekordbox", pioneer);
        cache.waitUntilPrefetchIdle();

        assert(fullPasses == 1);
        assert(fullCompleted == 0 && "the Full pass must not run to its end after the stick is pulled");
        assert(reporter.statted == 3 && "no file is stat'd after the one during which the stick was pulled");
        assert(reporter.afterPull == 0);
        const int stattedBeforeStop = reporter.statted;
        // Nothing of the cancelled pass is cached: a Full request reads
        // the sizes again, all of them.
        reporter.pull = [] {};
        reporter.pulled = true;
        const auto again = cache.stagedTracksFor("rekordbox", pioneer, Detail::Full);
        assert(fullPasses == 2 && fullCompleted == 1);
        assert(again.stage == Detail::Full && again.tracks.size() == fileCount && again.tracks[19].fileSizeBytes == 5);
        std::cout << "stage 14 (a stick pulled mid Full stops the prefetch within one file: " << stattedBeforeStop
                  << " stats ran, " << fileCount << " named) OK\n";
        fs::remove_all(root);
    }

    // Stage 15 (#58): plannedUnits() is what the passes still to run
    // would announce, from the catalog's row count: a unit per row for
    // the catalog read, for the two rekordbox catalogs' cue passes, and for Full's file
    // check (plus its cover check for the two catalogs that name cover
    // files); nothing for a stage the entry holds; unknown when the
    // catalog cannot be counted.
    {
        FakeReader reader;
        LibraryCatalogCache cache(reader.stageFn(), fixedMtime());
        cache.setCountFnForTesting([](const std::string &format, const std::string &path) -> std::optional<size_t> {
            if (path == "/nocount/PIONEER") {
                return std::nullopt;
            }
            return format == "engine" ? 20 : 10;
        });
        assert(cache.plannedUnits("rekordbox", stick, Detail::Tracks) == 10);
        assert(cache.plannedUnits("rekordbox", stick, Detail::Cues) == 20);
        assert(cache.plannedUnits("rekordbox", stick, Detail::Full) == 30);
        assert(cache.plannedUnits("engine", "/stick/Engine Library", Detail::Full) == 20 + 0 + 40);
        // OneLibrary's cue pass reads its analysis files (#59): a unit a row.
        assert(cache.plannedUnits("onelibrary", stick, Detail::Full) == 10 + 10 + 20);
        assert(!cache.plannedUnits("rekordbox", "/nocount/PIONEER", Detail::Full).has_value());
        assert(cache.countTracks("rekordbox", stick) == 10);

        cache.tracksFor("rekordbox", stick, Detail::Cues);
        assert(cache.plannedUnits("rekordbox", stick, Detail::Tracks) == 0 && "a stage the entry holds costs nothing");
        assert(cache.plannedUnits("rekordbox", stick, Detail::Cues) == 0);
        assert(cache.plannedUnits("rekordbox", stick, Detail::Full) == 10 && "only the file check is left");
        cache.tracksFor("rekordbox", stick, Detail::Full);
        assert(cache.plannedUnits("rekordbox", stick, Detail::Full) == 0);
        cache.invalidate("rekordbox", stick);
        assert(cache.plannedUnits("rekordbox", stick, Detail::Full) == 30 && "an invalidated entry is read again");
        std::cout << "stage 15 (plannedUnits counts the passes still to run) OK\n";
    }

    // Stage 15b (#67): a pass another thread is in (the prefetch, say) is
    // planned like one still to run, and a caller that waits for it sees
    // its progress on its own bar as it goes: no stall at the start, no
    // second read, and the bar ends on what was planned.
    {
        std::mutex m;
        std::condition_variable cv;
        bool halfway = false;
        bool release = false;
        std::atomic<int> reads{0};
        LibraryCatalogCache::StageFn stageFn = [&](Detail stage, const std::string &, const std::string &,
                                                   std::vector<seabass::domain::Track> &tracks, LibraryCatalogCache::StageNotes &,
                                                   seabass::application::ProgressReporter &progress, CancellationToken) {
            if (stage != Detail::Tracks) {
                return;
            }
            ++reads;
            progress.start("Scanning rekordbox tracks", 10);
            for (size_t i = 1; i <= 5; ++i) {
                progress.tick(i);
            }
            {
                std::unique_lock<std::mutex> lock(m);
                halfway = true;
                cv.notify_all();
                cv.wait(lock, [&] { return release; });
            }
            for (size_t i = 6; i <= 10; ++i) {
                progress.tick(i);
            }
            tracks.assign(10, seabass::domain::Track{});
        };
        LibraryCatalogCache cache(stageFn, fixedMtime());
        cache.setCountFnForTesting([](const std::string &, const std::string &) -> std::optional<size_t> { return 10; });

        auto first = std::async(std::launch::async, [&] { return cache.tracksFor("rekordbox", stick, Detail::Tracks); });
        {
            std::unique_lock<std::mutex> lock(m);
            cv.wait(lock, [&] { return halfway; });
        }
        assert(cache.plannedUnits("rekordbox", stick, Detail::Tracks) == 10 && "a pass in flight is planned, not counted as done");

        struct Recorder : seabass::application::ProgressReporter
        {
            std::mutex m;
            std::vector<std::pair<std::string, size_t>> starts;
            std::vector<size_t> ticks;
            void start(const std::string &label, size_t total) override
            {
                std::lock_guard<std::mutex> lock(m);
                starts.emplace_back(label, total);
            }
            void tick(size_t current) override
            {
                std::lock_guard<std::mutex> lock(m);
                ticks.push_back(current);
            }
            void finish() override {}
            void warn(const std::string &) override {}
        } recorder;
        auto waiter = std::async(std::launch::async, [&] { return cache.tracksFor("rekordbox", stick, Detail::Tracks, recorder); });
        // The waiter shows the pass as far as it has got before it ends.
        for (int i = 0; i < 100; ++i) {
            {
                std::lock_guard<std::mutex> lock(recorder.m);
                if (!recorder.ticks.empty()) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        {
            std::lock_guard<std::mutex> lock(recorder.m);
            assert(recorder.starts.size() == 1 && recorder.starts[0].second == 10 && "the waited pass's stretch, as planned");
            assert(!recorder.ticks.empty() && recorder.ticks.back() == 5 && "its progress while it is still running");
        }
        {
            std::lock_guard<std::mutex> lock(m);
            release = true;
            cv.notify_all();
        }
        assert(first.get().size() == 10 && waiter.get().size() == 10);
        assert(reads == 1 && "served from the pass it waited for, not read again");
        // Ticks seen at most every 100 ms: whatever the last one was, the
        // bar does not go back, and the waiter is served once the pass
        // commits. (It may or may not have seen the final tick.)
        for (size_t i = 1; i < recorder.ticks.size(); ++i) {
            assert(recorder.ticks[i] > recorder.ticks[i - 1] && "never backwards");
        }
        assert(cache.plannedUnits("rekordbox", stick, Detail::Tracks) == 0);
        std::cout << "stage 15b (a pass in flight is planned, and a waiter shows its progress) OK\n";
    }
}

// Counts are remembered (#58): a count opens the catalog, and for
// OneLibrary that is a key derivation no token interrupts, so a planner
// asking for one catalog twice, or a rescan of an unchanged stick, must
// not open it twice. Forgotten when the catalog file's mtime moves and
// by an invalidation; the token is looked at before a count is made.
void rememberedCountCases()
{
    const std::string stick = "/stick/PIONEER";
    FakeReader reader;
    std::chrono::system_clock::time_point mtime{};
    LibraryCatalogCache cache(reader.stageFn(), [&](const std::string &, const std::string &) { return mtime; });
    std::map<std::string, int> counted;
    bool failNext = false;
    cache.setCountFnForTesting([&](const std::string &format, const std::string &) -> std::optional<size_t> {
        ++counted[format];
        if (failNext) {
            failNext = false;
            return std::nullopt;
        }
        return 10;
    });

    // What every planner does: plannedUnits(), then the rows on top.
    assert(cache.plannedUnits("onelibrary", stick, Detail::Full) == 40);
    assert(cache.countTracks("onelibrary", stick) == 10);
    assert(cache.countTracks("onelibrary", "/stick/PIONEER/") == 10 && "every spelling is one count");
    assert(counted["onelibrary"] == 1);
    // A rescan of the same catalog file plans from the remembered count.
    cache.tracksFor("onelibrary", stick, Detail::Full);
    assert(cache.plannedUnits("onelibrary", stick, Detail::Full) == 0);
    assert(cache.countTracks("onelibrary", stick) == 10);
    assert(counted["onelibrary"] == 1 && "a rescan of an unchanged catalog counts nothing");

    // A remembered count is returned under a stopped token; a new one is
    // not made.
    CancellationToken stopped;
    stopped.cancel();
    assert(cache.countTracks("onelibrary", stick, stopped) == 10);
    bool threw = false;
    try {
        cache.countTracks("engine", "/stick/Engine Library", stopped);
    } catch (const OperationCancelled &) {
        threw = true;
    }
    assert(threw && counted["engine"] == 0 && "a stop is seen before a count is made");
    threw = false;
    try {
        cache.plannedUnits("rekordbox", stick, Detail::Full, stopped);
    } catch (const OperationCancelled &) {
        threw = true;
    }
    assert(threw && counted["rekordbox"] == 0);

    // The catalog file changed: counted again, once.
    mtime += 1s;
    assert(cache.countTracks("onelibrary", stick) == 10);
    assert(cache.countTracks("onelibrary", stick) == 10);
    assert(counted["onelibrary"] == 2);
    // An invalidation forgets it too, and its mirror.
    cache.invalidateWithOneLibraryMirror("rekordbox", stick);
    cache.countTracks("onelibrary", stick);
    assert(counted["onelibrary"] == 3);
    // A failed count is remembered too (a OneLibrary that does not open
    // is not tried twice), until the catalog file changes.
    failNext = true;
    assert(!cache.countTracks("rekordbox", stick).has_value());
    assert(!cache.countTracks("rekordbox", stick).has_value());
    assert(counted["rekordbox"] == 1);
    mtime += 1s;
    assert(cache.countTracks("rekordbox", stick) == 10);
    assert(counted["rekordbox"] == 2);
    std::cout << "stage 16 (a count is made once per catalog state, after a look at the token) OK\n";
}

// A player that stores a pad writes the track's analysis file and
// neither catalog file (XDJ-RX2, OMNIS-DUO, CDJ-3000X; see
// docs/onelibrary-format.md). An entry holding cues read from those
// files is stale once one of them moves, for rekordbox and OneLibrary
// both. Over a real copy of the fixture (no hard links: the touch must
// not reach the committed files).
void analysisFileFreshnessCases(const std::filesystem::path &fixture)
{
    namespace fs = std::filesystem;
    const fs::path root = seabass::testing::scratchRoot() / "library_catalog_cache_test_analysis";
    fs::remove_all(root);
    fs::create_directories(root);
    fs::copy(fixture / "rekordbox", root / "PIONEER", fs::copy_options::recursive);
    const std::string pioneer = seabass::pathToUtf8(root / "PIONEER");

    std::map<std::string, int> catalogReads;
    std::mutex readsMutex;
    const auto realStage = LibraryCatalogCache::realStageForTesting();
    LibraryCatalogCache cache(
        [&](Detail stage, const std::string &format, const std::string &path, std::vector<seabass::domain::Track> &tracks,
            LibraryCatalogCache::StageNotes &notes, seabass::application::ProgressReporter &progress,
            CancellationToken cancel) {
            if (stage == Detail::Tracks) {
                std::lock_guard<std::mutex> lock(readsMutex);
                ++catalogReads[format];
            }
            realStage(stage, format, path, tracks, notes, progress, std::move(cancel));
        },
        LibraryCatalogCache::realMtimeForTesting());

    // The window is measured on a clock the test moves: however slow the
    // machine, a read never outlasts it, and stepping past it never
    // sleeps.
    std::atomic<std::int64_t> fakeNowMs{1000000};
    cache.setNowFnForTesting(
        [&] { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(fakeNowMs.load())); });
    const auto pastTheWindow = [&] {
        fakeNowMs += (LibraryCatalogCache::analysisCheckWindow() + 1ms).count();
    };

    for (const std::string format : {"rekordbox", "onelibrary"}) {
        const auto tracks = cache.tracksFor(format, pioneer, Detail::Cues);
        assert(!tracks.empty());
        const auto named = std::find_if(tracks.begin(), tracks.end(),
                                        [](const seabass::domain::Track &t) { return !t.analysisFile.empty(); });
        assert(named != tracks.end());
        assert(catalogReads[format] == 1);

        // Within the window of the read, nothing is checked.
        int checks = cache.analysisChecksForTesting();
        assert(cache.plannedUnits(format, pioneer, Detail::Cues) == 0);
        cache.tracksFor(format, pioneer, Detail::Cues);
        assert(cache.analysisChecksForTesting() == checks && "a read just made stands for the window");

        // Past it, the first request for cues checks, timed, and the
        // requests of the same burst do not check again.
        pastTheWindow();
        const auto before = std::chrono::steady_clock::now();
        assert(cache.plannedUnits(format, pioneer, Detail::Cues) == 0);
        const auto took = std::chrono::steady_clock::now() - before;
        assert(cache.analysisChecksForTesting() == checks + 1);
        cache.tracksFor(format, pioneer, Detail::Cues);
        cache.plannedUnits(format, pioneer, Detail::Full);
        assert(cache.analysisChecksForTesting() == checks + 1 && "two requests within the window check once");
        assert(catalogReads[format] == 1 && "an unchanged stick is served from the cache");

        // A request for Tracks alone never checks, and past the window it
        // does not vouch for the cues it carries.
        pastTheWindow();
        checks = cache.analysisChecksForTesting();
        const auto tracksOnly = cache.stagedTracksFor(format, pioneer, Detail::Tracks);
        assert(cache.plannedUnits(format, pioneer, Detail::Tracks) == 0);
        assert(cache.analysisChecksForTesting() == checks && "a Tracks request does not check the analysis files");
        assert(tracksOnly.stage == Detail::Tracks && !tracksOnly.tracks.empty());

        // One analysis file moves, as a player's pad store moves it. The
        // track's .DAT: the legacy list a player reads hot cues from.
        const std::string rel = named->analysisFile.substr(named->analysisFile.find("/USBANLZ/") + 1);
        const fs::path dat = root / "PIONEER" / seabass::pathFromUtf8(rel);
        assert(fs::exists(dat));
        fs::last_write_time(dat, fs::last_write_time(dat) + 1min);
        pastTheWindow();
        // A stop gives the check up and leaves the entry as it is for
        // that call (the caller is stopping anyway).
        CancellationToken stopped;
        stopped.cancel();
        assert(cache.plannedUnits(format, pioneer, Detail::Cues, stopped) == 0
               && "a stopped check takes the entry as fresh");
        const auto planned = cache.plannedUnits(format, pioneer, Detail::Cues);
        assert(planned.has_value() && *planned > 0 && "a moved analysis file makes the entry stale");
        cache.tracksFor(format, pioneer, Detail::Cues);
        assert(catalogReads[format] == 2 && "and the next request reads the catalog and its cues again");
        assert(cache.plannedUnits(format, pioneer, Detail::Cues) == 0 && "and is fresh after that");
        std::cout << "stage 17 (" << format << ": a moved analysis file makes the entry stale, checked once per "
                  << "window and never for Tracks; the check took "
                  << std::chrono::duration_cast<std::chrono::microseconds>(took).count() << " us over "
                  << tracks.size() << " rows) OK\n";
    }
    fs::remove_all(root);
}

#ifdef SEABASS_TEST_HAVE_PROBE
// Engine's cues come with the Tracks stage, at the reader's 44.1 kHz
// guess for a row the player has not analysed yet (no sample rate
// recorded). The Cues stage asks each such row's file, through the
// stick's metadata cache, and only for rows with a cue or a loop. Over a
// copy of the fixture with a 48 kHz MP3 planted at the path of each of
// its 1317 .mp3 rows (the other 247 are .m4a and .mp4, left absent):
// of its 1212 rows with no trackData, 23 carry a cue or a loop, two of
// them .m4a files this copy does not have (ids 79 and 100). So the Tracks
// stage probes nothing, the Cues stage probes 23 files and caches the 21
// that answer, and every other row reads as the Tracks stage read it.
// Row 17 (Contents/a074e2.mp3) has one hot cue, pad 1 at sample
// 435487.5: 9875 ms at 44.1 kHz, 9072.65625 ms at 48 kHz.
void engineSampleRateCases(const std::filesystem::path &fixture)
{
    namespace fs = std::filesystem;
    using seabass::domain::Track;
    const fs::path stick = seabass::testing::scratchRoot() / "library_catalog_cache_test_sample_rates";
    fs::remove_all(stick);
    fs::create_directories(stick);
    fs::copy(fixture / "engine", stick / "Engine Library", fs::copy_options::recursive);
    const std::string engine = seabass::pathToUtf8(stick / "Engine Library");
    const fs::path metadataCache = stick / "Seabass" / "caches" / "metadata.jsonl";

    int catalogReads = 0;
    const auto realStage = LibraryCatalogCache::realStageForTesting();
    LibraryCatalogCache cache(
        [&](Detail stage, const std::string &format, const std::string &path, std::vector<Track> &tracks,
            LibraryCatalogCache::StageNotes &notes, seabass::application::ProgressReporter &progress,
            CancellationToken cancel) {
            if (stage == Detail::Tracks) {
                ++catalogReads;
            }
            realStage(stage, format, path, tracks, notes, progress, std::move(cancel));
        },
        LibraryCatalogCache::realMtimeForTesting());
    const auto byId = [](const std::vector<Track> &tracks, const std::string &id) -> const Track & {
        const auto it = std::find_if(tracks.begin(), tracks.end(), [&](const Track &t) { return t.sourceId == id; });
        assert(it != tracks.end());
        return *it;
    };

    const auto atTracks = cache.tracksFor("engine", engine, Detail::Tracks);
    assert(atTracks.size() == 1564);
    size_t planted = 0;
    for (const Track &t : atTracks) {
        if (seabass::pathFromUtf8(t.filePath).extension() == ".mp3") {
            seabass::test_fixture::mp3::writeMp3(seabass::pathFromUtf8(t.filePath), 10, true, 48000);
            ++planted;
        }
    }
    assert(planted == 1317 && "an MP3 at every .mp3 row's path: the rows not asked about could answer too");
    assert(!fs::exists(metadataCache) && "the Tracks stage opens no audio file");
    assert(byId(atTracks, "17").cues.size() == 1 && byId(atTracks, "17").cues[0].positionMs == 9875.0);

    const auto atCues = cache.tracksFor("engine", engine, Detail::Cues);
    assert(atCues.size() == atTracks.size());
    assert(catalogReads == 1 && "the Cues stage worked on the Tracks stage's rows");
    std::vector<std::string> lines;
    {
        std::ifstream in(metadataCache, std::ios::binary);
        assert(in && "the Cues stage saved what it probed");
        for (std::string line; std::getline(in, line);) {
            if (!line.empty()) {
                lines.push_back(line);
            }
        }
    }
    assert(lines.size() == 21 && "the 21 rows with cues, no rate and a readable file; nothing else");
    for (const std::string &line : lines) {
        assert(line.find("\"samplerate\":\"48000\"") != std::string::npos);
    }
    const auto row17 = std::find_if(lines.begin(), lines.end(), [](const std::string &line) {
        return line.find("\"path\":\"Engine Library/Contents/a074e2.mp3\"") != std::string::npos;
    });
    assert(row17 != lines.end());
    assert(byId(atCues, "17").cues.size() == 1 && byId(atCues, "17").cues[0].positionMs == 9072.65625);
    // No file, no answer: the guess stays.
    assert(byId(atCues, "79").cues.size() == 3);
    assert(byId(atCues, "79").cues[0].positionMs == byId(atTracks, "79").cues[0].positionMs);
    size_t moved = 0;
    for (size_t i = 0; i < atCues.size(); ++i) {
        assert(atCues[i].sourceId == atTracks[i].sourceId && atCues[i].cues.size() == atTracks[i].cues.size());
        bool same = true;
        for (size_t c = 0; c < atCues[i].cues.size(); ++c) {
            same = same && atCues[i].cues[c].positionMs == atTracks[i].cues[c].positionMs;
        }
        moved += same ? 0 : 1;
    }
    assert(moved == 21 && "exactly the rows whose file answered moved");
    assert(cache.tracksFor("engine", engine, Detail::Cues).size() == 1564 && catalogReads == 1
           && "looking up the rates left the entry fresh");
    std::cout << "stage 18 (Engine: the Cues stage asks the file's rate for the 23 rows with cues and no rate, "
                 "caches the 21 that answer, moves only theirs) OK\n";

    // The next pass answers from the cache, not the file: the entry says
    // 44.1 kHz for row 17's file (size and mtime as they are), and row 17
    // reads at 44.1 kHz. Nothing is probed, so nothing is written back.
    std::string edited;
    for (const std::string &line : lines) {
        std::string copy = line;
        if (copy == *row17) {
            const std::string at48k = "\"samplerate\":\"48000\"";
            copy.replace(copy.find(at48k), at48k.size(), "\"samplerate\":\"44100\"");
        }
        edited += copy + "\n";
    }
    {
        std::ofstream out(metadataCache, std::ios::binary | std::ios::trunc);
        out << edited;
    }
    cache.invalidate("engine", engine);
    const auto again = cache.tracksFor("engine", engine, Detail::Cues);
    assert(catalogReads == 2);
    assert(byId(again, "17").cues[0].positionMs == 9875.0 && "the cached rate, not the file's");
    {
        std::ifstream in(metadataCache, std::ios::binary);
        const std::string after((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        assert(after == edited && "every answer came from the cache: no probe, no save");
    }
    std::cout << "stage 19 (Engine: a cached rate is used without opening the file) OK\n";
    cache.invalidate("engine", engine);
    fs::remove_all(stick);
}
#endif

}  // namespace

int main(int argc, char **argv)
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
    rememberedCountCases();
    if (argc > 1) {
        analysisFileFreshnessCases(seabass::pathFromUtf8(argv[1]));
#ifdef SEABASS_TEST_HAVE_PROBE
        engineSampleRateCases(seabass::pathFromUtf8(argv[1]));
#else
        std::cout << "SKIP engineSampleRateCases: this build has no metadata probe\n";
#endif
    } else {
        std::cerr << "no fixture given: the analysis-file freshness case did not run\n";
        return 1;
    }

    std::cout << "All library_catalog_cache tests passed.\n";
    return 0;
}
