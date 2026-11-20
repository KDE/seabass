// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <sys/prctl.h>
#include <chrono>
#include <thread>
#endif
#if defined(__APPLE__)
#include <libproc.h>
#include <sys/param.h>
#endif

#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/system/rekordbox_process_detector.hpp"

using namespace seabass::infrastructure::system;

namespace
{

// Self-detection: this very test process should find itself running,
// proving the scanner actually inspects real process state rather than
// trivially returning false for everything. Mirrors how the production
// detector itself reads each platform's process name (see
// rekordbox_process_detector.cpp) -- /proc/<pid>/comm's 15-byte
// truncation on Linux, the bare name (detector appends ".exe" itself) on
// Windows, proc_name() on macOS.
std::string selfProcessNameForDetector()
{
#if defined(_WIN32)
    wchar_t selfExe[MAX_PATH] = {};
    DWORD n = ::GetModuleFileNameW(nullptr, selfExe, MAX_PATH);
    assert(n > 0 && n < MAX_PATH);
    std::string baseName = seabass::pathToUtf8(std::filesystem::path(std::wstring(selfExe, n)).filename());
    size_t dot = baseName.rfind(".exe");
    if (dot != std::string::npos && dot == baseName.size() - 4) {
        baseName = baseName.substr(0, dot);
    }
    return baseName;
#elif defined(__APPLE__)
    char name[2 * MAXCOMLEN + 1] = {};
    int n = proc_name(getpid(), name, sizeof(name));
    assert(n > 0);
    return std::string(name, static_cast<size_t>(n));
#else
    char selfExe[4096] = {};
    ssize_t n = readlink("/proc/self/exe", selfExe, sizeof(selfExe) - 1);
    assert(n > 0);
    std::string exePath(selfExe, static_cast<size_t>(n));
    std::string baseName = exePath.substr(exePath.find_last_of('/') + 1);
    if (baseName.size() > 15) {
        baseName = baseName.substr(0, 15);
    }
    return baseName;
#endif
}

}  // namespace

int main()
{
    assert(isProcessRunning(selfProcessNameForDetector()));
    std::cout << "case 1 (self-detection) OK\n";

    assert(!isProcessRunning("definitely-not-a-real-process-xyz123"));
    std::cout << "case 2 (nonexistent process not detected) OK\n";

    // Not a hard assertion -- rekordbox could theoretically be running via
    // Wine on this machine -- but flagging it is more useful than silently
    // passing if the detector is ever broken in a way that always returns
    // true.
    if (isRekordboxRunning()) {
        std::cerr << "warning: isRekordboxRunning() returned true in the test environment\n";
    }
    std::cout << "case 3 (isRekordboxRunning wrapper runs without error) OK\n";

    // A process renamed after fork is seen by name, and unseen once it
    // exits: the real scan, against a process this test owns. Not under a
    // DJ application's name: while this ran, the release rig, a live lane
    // or the installed Seabass on the same machine would see it too and
    // refuse its writes, which is the very thing tests/nothing_else_running.hpp
    // keeps the suite from depending on. So a name of the same shape as
    // "Engine DJ" (two words, a space, under comm's 15 bytes) and nobody's
    // product, and PR_SET_PDEATHSIG so a killed test cannot leave it behind.
    //
    // Linux only, because prctl() is how a process renames itself here.
#if defined(__linux__)
    {
        static const char *const probeName = "Seabass Probe";
        assert(!isProcessRunning(probeName));
        const pid_t parent = ::getpid();
        const pid_t child = ::fork();
        assert(child >= 0);
        if (child == 0) {
            ::prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
            if (::getppid() != parent) {
                ::_exit(0);
            }
            ::prctl(PR_SET_NAME, probeName, 0, 0, 0);
            for (;;) {
                ::pause();
            }
            ::_exit(0);
        }

        // The rename happens after the fork returns in the parent, so
        // wait for it rather than racing it.
        bool seen = false;
        for (int attempt = 0; attempt < 100 && !seen; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            seen = isProcessRunning(probeName);
        }

        // Killed BEFORE anything is asserted: a failing assert aborts and
        // would leave the child in pause().
        ::kill(child, SIGKILL);
        int status = 0;
        ::waitpid(child, &status, 0);

        assert(seen && "a renamed child process was running and the scan did not see it");
        bool gone = false;
        for (int attempt = 0; attempt < 100 && !gone; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            gone = !isProcessRunning(probeName);
        }
        assert(gone);
        std::cout << "case 4 (a renamed child process is seen, and unseen once it exits) OK\n";
    }
#endif

    // The DJ names on top of the scan, from a scan this test names: that
    // "Engine DJ" in the process list makes the guard's checks fire and
    // name it, and that they stop once it is gone.
    setProcessScanForTesting([](const std::string &name) {
        return name == "Engine DJ";
    });
    assert(processScanIsOverriddenForTesting());
    assert(isEngineDjRunning());
    assert(!isRekordboxRunning());
    assert(isConflictingDjSoftwareRunning());
    assert(conflictingDjSoftwareName() == "Engine DJ");
    setProcessScanForTesting([](const std::string &) {
        return false;
    });
    assert(!isConflictingDjSoftwareRunning());
    assert(conflictingDjSoftwareName().empty());
    setProcessScanForTesting(nullptr);
    assert(!processScanIsOverriddenForTesting());
    std::cout << "case 5 (Engine DJ in the process list is seen and named, and not once gone) OK\n";

    std::cout << "all cases passed\n";
    return 0;
}
