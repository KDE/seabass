// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
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

    std::cout << "All library_catalog_cache tests passed.\n";
    return 0;
}
