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
#include <unistd.h>
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
// Windows.
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

    std::cout << "all cases passed\n";
    return 0;
}
