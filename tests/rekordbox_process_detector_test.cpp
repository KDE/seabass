// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
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
    char selfExe[MAX_PATH] = {};
    DWORD n = ::GetModuleFileNameA(nullptr, selfExe, sizeof(selfExe));
    assert(n > 0 && n < sizeof(selfExe));
    std::string exePath(selfExe, n);
    std::string baseName = exePath.substr(exePath.find_last_of("\\/") + 1);
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

    // The guard this exists for, actually seen to fire. Everything above
    // proves the scanner reads real process state; none of it proves it
    // would recognise the one name the feature is about, and a guard
    // that has never been seen to close is not known to close. So: a
    // child process really called "Engine DJ".
    //
    // Linux only, because prctl() is how a process renames itself here.
    // What this does NOT cover is the spelling on the other two
    // platforms -- "Engine DJ.exe" on Windows, the bundle name on macOS
    // -- which only a machine with Engine DJ installed can settle.
#if defined(__linux__)
    if (isEngineDjRunning()) {
        std::cerr << "warning: Engine DJ appears to be running here, so the child-process case is skipped\n";
    } else {
        const pid_t child = ::fork();
        assert(child >= 0);
        if (child == 0) {
            ::prctl(PR_SET_NAME, "Engine DJ", 0, 0, 0);
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
            seen = isEngineDjRunning();
        }
        const bool conflicting = isConflictingDjSoftwareRunning();
        const std::string named = conflictingDjSoftwareName();

        // Killed BEFORE anything is asserted. A failing assert aborts,
        // an abort runs no more of this function, and the child sits in
        // pause() for ever -- which on this machine means every later
        // run of this test finds an "Engine DJ" already running and
        // quietly skips the case that just failed.
        ::kill(child, SIGKILL);
        int status = 0;
        ::waitpid(child, &status, 0);

        assert(seen && "a process called \"Engine DJ\" was running and the guard did not see it");
        assert(conflicting);
        assert(named == "Engine DJ");

        // And it stops saying so once the process is gone, or the guard
        // would refuse every write for ever after one run of Engine DJ.
        bool gone = false;
        for (int attempt = 0; attempt < 100 && !gone; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            gone = !isEngineDjRunning();
        }
        assert(gone);
        assert(conflictingDjSoftwareName().empty());
        std::cout << "case 4 (a process called \"Engine DJ\" is seen, and unseen once it exits) OK\n";
    }
#endif

    std::cout << "all cases passed\n";
    return 0;
}
