// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <atomic>
#include <functional>

#include "application/ports/removable_media_monitor.hpp"

namespace seabass::infrastructure::media
{

// Watches DiskArbitration for disks appearing, disappearing, and being
// mounted or unmounted, on a private serial dispatch queue. Registering
// also reports every disk already attached, so callers see a burst of
// changes right after start() -- MediaController debounces them.
class MacDiskArbitrationMonitor : public application::RemovableMediaMonitor
{
public:
    MacDiskArbitrationMonitor() = default;
    ~MacDiskArbitrationMonitor() override;

    void start(std::function<void()> onChange) override;
    void stop() override;

    // Called by the DiskArbitration callbacks, on the monitor's queue.
    void notify();

private:

    // DASessionRef and dispatch_queue_t, kept opaque so this header does
    // not pull in the Apple frameworks.
    void *m_session = nullptr;
    void *m_queue = nullptr;
    std::atomic<bool> m_running{false};
    std::function<void()> m_onChange;
};

}  // namespace seabass::infrastructure::media
