// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

#include "infrastructure/backup/stick_write_lock.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::backup;
namespace fs = std::filesystem;

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_stick_write_lock_test";
    fs::remove_all(root);
    fs::create_directories(root);
    std::string lockPath = (root / "Seabass" / "backups" / ".write.lock").string();

    // Basic acquire/release: creating and destroying a lock cleanly is
    // not itself an error, and does not leave anything held.
    {
        { StickWriteLock lock(lockPath); }
        { StickWriteLock lock(lockPath); }  // would throw if the first leaked its hold
        std::cout << "case 1 (acquire, release, re-acquire) OK\n";
    }

    // Two independent open()s of the same lock file in this same process
    // and thread must still contend -- this is the property the whole
    // in-process race fix (two controllers racing each other) depends on.
    {
        StickWriteLock first(lockPath);
        bool threw = false;
        try {
            StickWriteLock second(lockPath);
        } catch (const StickBusyError &) {
            threw = true;
        }
        assert(threw);
        std::cout << "case 2 (a second same-process lock on the same path is refused) OK\n";
    }

    // Releasing the first (scope exit) must let a new lock through.
    {
        { StickWriteLock first(lockPath); }
        StickWriteLock second(lockPath);  // would throw if case 2's lock leaked past its scope
        std::cout << "case 3 (lock released on destruction, a later lock succeeds) OK\n";
    }

    // Cross-thread: a lock held on a background thread must be visible to
    // (and refused for) this thread -- proving this is real OS-level
    // exclusion, not just a same-thread guard.
    {
        std::atomic<bool> holderReady{false};
        std::atomic<bool> releaseHolder{false};
        std::thread holder([&]() {
            StickWriteLock lock(lockPath);
            holderReady = true;
            while (!releaseHolder) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        while (!holderReady) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        bool threw = false;
        try {
            StickWriteLock lock(lockPath);
        } catch (const StickBusyError &) {
            threw = true;
        }
        assert(threw);

        releaseHolder = true;
        holder.join();

        StickWriteLock lock(lockPath);  // now free
        std::cout << "case 4 (cross-thread: held lock is refused, released lock is available) OK\n";
    }

    // Different stick, different lock file -- must never contend with
    // each other, or a write to one stick would needlessly block a write
    // to an unrelated one.
    {
        std::string otherLockPath = (root / "other-stick" / "Seabass" / "backups" / ".write.lock").string();
        StickWriteLock a(lockPath);
        StickWriteLock b(otherLockPath);  // would throw if locks weren't scoped per-path
        std::cout << "case 5 (locks for different sticks don't contend) OK\n";
    }

    fs::remove_all(root);
    std::cout << "all cases passed\n";
    return 0;
}
