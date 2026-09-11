// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <atomic>
#include <functional>
#include <thread>

#include "application/ports/removable_media_monitor.hpp"

struct udev;
struct udev_monitor;

namespace seabass::infrastructure::media
{

// Watches udev for block-device hotplug events (USB sticks inserted,
// removed, mounted, or unmounted) on a dedicated background thread, so
// callers can auto-refresh instead of requiring a manual "Refresh" click.
class LinuxUdevMediaMonitor : public application::RemovableMediaMonitor
{
public:
    LinuxUdevMediaMonitor() = default;
    ~LinuxUdevMediaMonitor() override;

    void start(std::function<void()> onChange) override;
    void stop() override;

private:
    void run();

    udev *m_udev = nullptr;
    udev_monitor *m_monitor = nullptr;
    int m_wakeFds[2] = {-1, -1};
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::function<void()> m_onChange;
};

}  // namespace seabass::infrastructure::media
