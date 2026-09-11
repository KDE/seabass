// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// FileLibraryEditLockStore: the local edit-lock cookie every instance
// consults before editing a library. Liveness and clock are injected so
// "the owner is dead" and "the foreign heartbeat is old" are exact;
// case 9 uses the real liveness against a forked child for the one
// thing a fake cannot prove.

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>

#include "infrastructure/local/file_library_edit_lock_store.hpp"
#include "infrastructure/local/flat_json.hpp"
#include "infrastructure/system/process_liveness.hpp"

#if !defined(_WIN32)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "scratch_path.hpp"

using namespace seabass::infrastructure::local;
using seabass::application::EditLockStatus;
using seabass::application::LibraryEditLock;
namespace fs = std::filesystem;
namespace sys = seabass::infrastructure::system;

namespace
{

LibraryEditLock lockFor(const std::string &instance, std::int64_t pid, const std::string &host = "thishost")
{
    LibraryEditLock lock;
    lock.libraryId = "EB9F-F032";
    lock.instanceId = instance;
    lock.hostname = host;
    lock.stickLabel = "A3";
    lock.mountPoint = "/media/A3";
    lock.pid = pid;
    lock.processStartId = 777;
    return lock;
}

}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_library_edit_lock_store_test";
    fs::remove_all(root);
    fs::create_directories(root);

    std::set<std::int64_t> alivePids = {100, 200};
    std::int64_t now = 1'700'000'000;
    auto liveness = [&](std::int64_t pid, std::uint64_t) { return alivePids.count(pid) > 0; };
    auto clock = [&]() { return now; };
    FileLibraryEditLockStore store(root / "edit-locks", liveness, clock, "thishost");

    // 1. Round trip through the on-disk format, including a label with
    //    characters that need escaping.
    {
        LibraryEditLock lock = lockFor("A", 100);
        lock.stickLabel = "My \"Stick\"\\n";
        auto back = FileLibraryEditLockStore::deserialize(FileLibraryEditLockStore::serialize(lock));
        assert(back);
        assert(back->libraryId == "EB9F-F032" && back->instanceId == "A" && back->pid == 100);
        assert(back->processStartId == 777 && back->stickLabel == lock.stickLabel);
        assert(!FileLibraryEditLockStore::deserialize("{not json"));
        assert(!FileLibraryEditLockStore::deserialize("{\"instanceId\":\"A\"}"));  // no libraryId
        auto numbers = parseFlatObject("{\"a\": 12, \"b\":true,\"c\":\"\\u00e9\"}");
        assert(numbers && (*numbers)["a"] == "12" && (*numbers)["b"] == "true" && (*numbers)["c"] == "\xC3\xA9");
        std::cout << "case 1 (serialisation) OK\n";
    }

    // 2. Free -> acquire -> probes from both sides -> listAll.
    {
        assert(store.probe("EB9F-F032", "A").status == EditLockStatus::Free);
        assert(store.tryAcquire(lockFor("A", 100)));
        auto mine = store.probe("EB9F-F032", "A");
        assert(mine.status == EditLockStatus::HeldByThisInstance);
        auto theirs = store.probe("EB9F-F032", "B");
        assert(theirs.status == EditLockStatus::HeldByOther);
        assert(theirs.holder && theirs.holder->instanceId == "A" && theirs.holder->stickLabel == "A3");
        assert(theirs.holder->heartbeatUnix == now && !theirs.holder->startedAtUtc.empty());
        assert(store.listAll().size() == 1);
        std::cout << "case 2 (acquire and probe) OK\n";
    }

    // 3. A live holder refuses a second instance; the file is untouched.
    {
        assert(!store.tryAcquire(lockFor("B", 200)));
        assert(store.probe("EB9F-F032", "B").holder->instanceId == "A");
        std::cout << "case 3 (second instance refused) OK\n";
    }

    // 4. release() only ever removes our own cookie; re-acquiring our own
    //    is fine (a new session in the same instance).
    {
        store.release("EB9F-F032", "B");
        assert(store.probe("EB9F-F032", "B").status == EditLockStatus::HeldByOther);
        assert(store.tryAcquire(lockFor("A", 100)));
        store.release("EB9F-F032", "A");
        assert(store.probe("EB9F-F032", "A").status == EditLockStatus::Free);
        assert(store.listAll().empty());
        std::cout << "case 4 (release) OK\n";
    }

    // 5. A cookie whose process is gone is Stale and gets replaced.
    {
        assert(store.tryAcquire(lockFor("A", 100)));
        alivePids.erase(100);
        auto probe = store.probe("EB9F-F032", "B");
        assert(probe.status == EditLockStatus::Stale && probe.holder && probe.holder->instanceId == "A");
        assert(store.tryAcquire(lockFor("B", 200)));
        assert(store.probe("EB9F-F032", "C").holder->instanceId == "B");
        alivePids.insert(100);
        std::cout << "case 5 (stale cookie replaced) OK\n";
    }

    // 6. heartbeat() moves only the heartbeat, only for the owner.
    {
        now += 100;
        store.heartbeat("EB9F-F032", "A");  // not the owner: nothing changes
        assert(store.probe("EB9F-F032", "C").holder->heartbeatUnix == now - 100);
        store.heartbeat("EB9F-F032", "B");
        auto probe = store.probe("EB9F-F032", "C");
        assert(probe.holder->heartbeatUnix == now && probe.holder->instanceId == "B");
        std::cout << "case 6 (heartbeat) OK\n";
    }

    // 7. forceRemove() ("Remove Lock") ignores ownership.
    {
        store.forceRemove("EB9F-F032");
        assert(store.probe("EB9F-F032", "C").status == EditLockStatus::Free);
        std::cout << "case 7 (forceRemove) OK\n";
    }

    // 8. A cookie from another host cannot be checked by pid: it is held
    //    while its heartbeat is fresh, stale once ten minutes have passed.
    {
        assert(store.tryAcquire(lockFor("X", 4242, "otherhost")));
        alivePids.erase(4242);  // irrelevant: the pid lives elsewhere
        assert(store.probe("EB9F-F032", "A").status == EditLockStatus::HeldByOther);
        now += FileLibraryEditLockStore::ForeignHostStaleAfterSeconds + 1;
        assert(store.probe("EB9F-F032", "A").status == EditLockStatus::Stale);
        assert(store.tryAcquire(lockFor("A", 100)));
        store.release("EB9F-F032", "A");
        std::cout << "case 8 (foreign host heartbeat) OK\n";
    }

    // 9. An unparseable cookie (torn, or mid-write by another instance)
    //    counts as held while young, stale once it is old.
    {
        fs::path path = store.pathFor("EB9F-F032");
        { std::ofstream(path) << ""; }
        assert(store.probe("EB9F-F032", "A").status == EditLockStatus::HeldByOther);
        assert(!store.tryAcquire(lockFor("A", 100)));
        fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(1));
        assert(store.probe("EB9F-F032", "A").status == EditLockStatus::Stale);
        assert(store.tryAcquire(lockFor("A", 100)));
        store.release("EB9F-F032", "A");
        std::cout << "case 9 (unparseable cookie) OK\n";
    }

    // 10. Real process liveness: this process is alive with its own start
    //     id, dead with a wrong one (where start ids exist), and a pid
    //     nothing can have is dead.
    {
        std::int64_t self = sys::currentPid();
        std::uint64_t start = sys::currentProcessStartId();
        assert(sys::isProcessAlive(self, start));
        assert(sys::isProcessAlive(self, 0));
        if (start != 0) {
            assert(!sys::isProcessAlive(self, start + 1));
        }
        assert(!sys::isProcessAlive(1LL << 30, 0));
        assert(!sys::isProcessAlive(0, 0));
        assert(!sys::hostName().empty());
        std::cout << "case 10 (process liveness) OK\n";
    }

#if !defined(_WIN32)
    // 11. Two real processes: a forked child acquires with its real pid and
    //     the real liveness check; after it is killed its cookie is stale.
    {
        FileLibraryEditLockStore real(root / "real-locks");
        pid_t child = ::fork();
        assert(child >= 0);
        if (child == 0) {
            FileLibraryEditLockStore childStore(root / "real-locks");
            LibraryEditLock lock;
            lock.libraryId = "CHILD-LIB";
            lock.instanceId = "child";
            lock.pid = sys::currentPid();
            lock.processStartId = sys::currentProcessStartId();
            if (!childStore.tryAcquire(lock)) {
                _exit(2);
            }
            ::pause();
            _exit(0);
        }
        // Wait for the child's cookie to appear.
        for (int i = 0; i < 200 && real.probe("CHILD-LIB", "parent").status == EditLockStatus::Free; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        auto held = real.probe("CHILD-LIB", "parent");
        assert(held.status == EditLockStatus::HeldByOther);
        assert(held.holder && held.holder->pid == child);
        LibraryEditLock mine;
        mine.libraryId = "CHILD-LIB";
        mine.instanceId = "parent";
        mine.pid = sys::currentPid();
        mine.processStartId = sys::currentProcessStartId();
        assert(!real.tryAcquire(mine));

        ::kill(child, SIGKILL);
        int status = 0;
        ::waitpid(child, &status, 0);
        assert(real.probe("CHILD-LIB", "parent").status == EditLockStatus::Stale);
        assert(real.tryAcquire(mine));
        assert(real.probe("CHILD-LIB", "parent").status == EditLockStatus::HeldByThisInstance);
        real.release("CHILD-LIB", "parent");
        std::cout << "case 11 (forked holder, killed) OK\n";
    }
#endif

    fs::remove_all(root);
    std::cout << "library_edit_lock_store_test: all cases passed\n";
    return 0;
}
