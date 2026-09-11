// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/media/windows_removable_media_monitor.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>

namespace seabass::infrastructure::media
{

WindowsRemovableMediaMonitor::~WindowsRemovableMediaMonitor()
{
    stop();
}

void WindowsRemovableMediaMonitor::start(std::function<void()> onChange)
{
    if (m_running) {
        return;
    }
    m_onChange = std::move(onChange);
    m_lastDriveMask = ::GetLogicalDrives();
    m_running = true;
    m_thread = std::thread(&WindowsRemovableMediaMonitor::run, this);
}

void WindowsRemovableMediaMonitor::stop()
{
    m_running = false;
    // Without this, stop() only returns once run()'s sleep_for(2s) below
    // happens to wake up on its own -- MediaController and
    // FormatUsbController are both root-level QML_ELEMENTs (see Main.qml),
    // so every one of these blocked destructors runs on the Qt main
    // thread during app shutdown, and it was possible to see the window
    // sit unresponsive for up to 2s per monitor (up to ~4s total) after
    // the user asked the app to close.
    m_wakeCv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void WindowsRemovableMediaMonitor::run()
{
    while (m_running) {
        {
            std::unique_lock<std::mutex> lock(m_wakeMutex);
            m_wakeCv.wait_for(lock, std::chrono::seconds(2), [this] { return !m_running.load(); });
        }
        if (!m_running) {
            break;
        }
        std::uint32_t driveMask = ::GetLogicalDrives();
        if (driveMask != m_lastDriveMask) {
            m_lastDriveMask = driveMask;
            if (m_onChange) {
                m_onChange();
            }
        }
    }
}

}  // namespace seabass::infrastructure::media
