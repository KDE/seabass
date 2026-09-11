// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/media/linux_udev_media_monitor.hpp"

#include <libudev.h>
#include <poll.h>
#include <unistd.h>

namespace seabass::infrastructure::media
{

LinuxUdevMediaMonitor::~LinuxUdevMediaMonitor()
{
    stop();
}

void LinuxUdevMediaMonitor::start(std::function<void()> onChange)
{
    if (m_running.exchange(true)) {
        return;
    }
    m_onChange = std::move(onChange);

    if (pipe(m_wakeFds) != 0) {
        m_running = false;
        return;
    }

    m_udev = udev_new();
    m_monitor = udev_monitor_new_from_netlink(m_udev, "udev");
    udev_monitor_filter_add_match_subsystem_devtype(m_monitor, "block", nullptr);
    udev_monitor_enable_receiving(m_monitor);

    m_thread = std::thread(&LinuxUdevMediaMonitor::run, this);
}

void LinuxUdevMediaMonitor::run()
{
    int monitorFd = udev_monitor_get_fd(m_monitor);

    while (m_running.load()) {
        pollfd fds[2] = {
            {monitorFd, POLLIN, 0},
            {m_wakeFds[0], POLLIN, 0},
        };
        if (poll(fds, 2, -1) <= 0) {
            continue;
        }
        if (fds[1].revents & POLLIN) {
            break;  // stop() woke us up
        }
        if (fds[0].revents & POLLIN) {
            if (udev_device *dev = udev_monitor_receive_device(m_monitor)) {
                udev_device_unref(dev);
                if (m_onChange) {
                    m_onChange();
                }
            }
        }
    }
}

void LinuxUdevMediaMonitor::stop()
{
    if (!m_running.exchange(false)) {
        return;
    }
    if (m_wakeFds[1] >= 0) {
        char byte = 0;
        [[maybe_unused]] ssize_t written = write(m_wakeFds[1], &byte, 1);
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_monitor) {
        udev_monitor_unref(m_monitor);
        m_monitor = nullptr;
    }
    if (m_udev) {
        udev_unref(m_udev);
        m_udev = nullptr;
    }
    if (m_wakeFds[0] >= 0) {
        close(m_wakeFds[0]);
    }
    if (m_wakeFds[1] >= 0) {
        close(m_wakeFds[1]);
    }
    m_wakeFds[0] = m_wakeFds[1] = -1;
}

}  // namespace seabass::infrastructure::media
