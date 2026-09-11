// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>

namespace seabass::application
{

// Port for reacting to removable-media hotplug events (a USB stick inserted,
// removed, mounted, or unmounted) without the caller needing to know how
// that's observed on the host OS (udev on Linux, ...).
class RemovableMediaMonitor
{
public:
    virtual ~RemovableMediaMonitor() = default;

    // onChange may be invoked from a background thread whenever a relevant
    // block device appears, disappears, or changes mount state. Callers
    // that need the result on a particular thread must marshal it
    // themselves.
    virtual void start(std::function<void()> onChange) = 0;
    virtual void stop() = 0;
};

}  // namespace seabass::application
