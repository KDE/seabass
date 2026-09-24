// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
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

    // Removing the lock file: gone afterwards, the path locks again (a new
    // file), and a lock still held on the old, deleted file is not the
    // lock on the path.
    {
        std::string removedPath = (root / "discarded" / "stick.zip.lock").string();
        {
            StickWriteLock lock(removedPath);
            assert(fs::exists(removedPath));
            lock.releaseAndRemoveFile();
            assert(!fs::exists(removedPath));
        }
        StickWriteLock again(removedPath);  // releaseAndRemoveFile left nothing held
        assert(fs::exists(removedPath));
        std::cout << "case 6 (releaseAndRemoveFile removes the file and holds nothing) OK\n";
    }

    // A backup in a folder named outside the Windows ANSI code page. The
    // lock took a std::string and opened it with CreateFileA, so on Windows
    // this threw "Could not open stick lock file" and the backup, compact
    // or restore behind it never started. Linux has no code page to fall
    // outside of, so here it pins the path-based contract; on Windows it
    // is the regression test.
    {
        const std::u8string folder = u8"Sicherung \u00c4rger \u65e5\u672c \u041a\u0438\u043d\u043e \U0001F3A7";
        const fs::path unicodeLock = root / fs::path(folder) / ".write.lock";
        {
            StickWriteLock first(unicodeLock);
            assert(fs::exists(unicodeLock));
            bool threw = false;
            try {
                StickWriteLock second(unicodeLock);
            } catch (const StickBusyError &e) {
                threw = true;
                // The message names the folder readably, as UTF-8.
                const std::u8string u8 = fs::path(folder).u8string();
                assert(std::string(e.what()).find(std::string(reinterpret_cast<const char *>(u8.data()), u8.size()))
                       != std::string::npos);
            }
            assert(threw);
            first.releaseAndRemoveFile();
            assert(!fs::exists(unicodeLock));
        }
        std::cout << "case 7 (a lock in a folder named outside the code page) OK\n";
    }

    fs::remove_all(root);
    std::cout << "all cases passed\n";
    return 0;
}
