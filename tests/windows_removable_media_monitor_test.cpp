// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// WindowsRemovableMediaMonitor::stop() used to only return once run()'s
// sleep_for(2s) poll happened to wake up on its own. MediaController and
// FormatUsbController are both root-level QML_ELEMENTs (see Main.qml), so
// every one of their destructors -- including this monitor's stop() --
// runs on the Qt main thread during app shutdown: closing the app could
// sit unresponsive for up to ~2s per monitor. Fixed with a condition
// variable stop() can wake immediately; this pins that down so it can't
// silently regress back to the blocking poll.
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

#include "infrastructure/media/windows_removable_media_monitor.hpp"

using seabass::infrastructure::media::WindowsRemovableMediaMonitor;

int main()
{
    WindowsRemovableMediaMonitor monitor;
    monitor.start([]() {});
    // Real usage never calls stop() a moment after start(): the monitor
    // runs for the app's whole lifetime, so by the time stop() is called
    // the background thread is essentially guaranteed to be blocked
    // inside its poll wait. Called immediately like this instead, stop()
    // can race ahead of the new thread ever reaching that wait at all --
    // std::thread's constructor returning is not the same as the OS
    // having scheduled the thread function's first instruction -- and
    // pass even against the unfixed sleep_for(2s), because m_running is
    // already false before the loop's first check. Sleeping here first
    // pins the thread inside the wait before timing stop().
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const auto begin = std::chrono::steady_clock::now();
    monitor.stop();
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();

    // Generous relative to the old ~2000ms floor: any regression back to
    // waiting out the poll interval fails this by more than 10x, nothing
    // this machine's own scheduling jitter could produce by accident.
    assert(elapsedMs < 500 && "stop() must return promptly, not wait out the poll interval");

    // stop() must also be safe to call again (dtor calls it too) and not
    // hang or double-join.
    monitor.stop();

    std::cout << "stop() returned in " << elapsedMs << "ms\n";
    return 0;
}
