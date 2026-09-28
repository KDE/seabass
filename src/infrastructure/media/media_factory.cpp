// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/media/media_factory.hpp"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string_view>

#if defined(_WIN32)
#include "infrastructure/media/windows_removable_media_locator.hpp"
#include "infrastructure/media/windows_removable_media_monitor.hpp"
#include "infrastructure/media/windows_removable_media_mounter.hpp"
#include "infrastructure/media/windows_usb_formatter.hpp"
#elif defined(__APPLE__)
#include "infrastructure/media/diskutil_media_mounter.hpp"
#include "infrastructure/media/macos_disk_arbitration_monitor.hpp"
#include "infrastructure/media/macos_removable_media_locator.hpp"
#include "infrastructure/media/macos_usb_formatter.hpp"
#else
#include "infrastructure/media/linux_removable_media_locator.hpp"
#include "infrastructure/media/linux_udev_media_monitor.hpp"
#include "infrastructure/media/linux_usb_formatter.hpp"
#include "infrastructure/media/udisksctl_media_mounter.hpp"
#endif

namespace seabass::infrastructure::media
{

namespace
{

// A locator that finds nothing, for a process that must not see the
// sticks plugged into this computer: the test suites. tst_PagesCompile
// builds the home page on the real MediaController, and the home page
// scans the first stick it sees, whose Full read then wrote a duration
// cache onto two rig sticks in the middle of a shakedown (2026-09-28).
// A test suite that reads or writes whatever happens to be inserted is
// one plugged-in DJ stick away from a real loss, so the suites run with
// SEABASS_IGNORE_REMOVABLE_MEDIA set (CMakeLists.txt, the QML lanes) and
// every locator this factory hands out then answers empty. The live suite
// (tests/qml-live) wants the real sticks and does not set it.
class StandInMonitor;

struct StandIns
{
    std::mutex mutex;
    std::function<std::vector<application::DetectedStick>()> sticks;
    std::vector<StandInMonitor *> monitors;
};

StandIns &standIns()
{
    // Never destroyed: a detect on a worker may outlive main().
    static StandIns *standIns = new StandIns();
    return *standIns;
}

// Finds nothing, or what a test stood in for a stick
// (setStandInSticksForTesting): never a real one.
class NoRemovableMediaLocator : public application::RemovableMediaLocator
{
public:
    std::vector<application::DetectedStick> detect() override
    {
        std::function<std::vector<application::DetectedStick>()> sticks;
        {
            std::lock_guard<std::mutex> lock(standIns().mutex);
            sticks = standIns().sticks;
        }
        return sticks ? sticks() : std::vector<application::DetectedStick>{};
    }
};

// Hears no device at all, only announceMediaChangeForTesting().
class StandInMonitor : public application::RemovableMediaMonitor
{
public:
    ~StandInMonitor() override { stop(); }
    void start(std::function<void()> onChange) override
    {
        std::lock_guard<std::mutex> lock(standIns().mutex);
        m_onChange = std::move(onChange);
        auto &monitors = standIns().monitors;
        if (std::find(monitors.begin(), monitors.end(), this) == monitors.end()) {
            monitors.push_back(this);
        }
    }
    void stop() override
    {
        std::lock_guard<std::mutex> lock(standIns().mutex);
        auto &monitors = standIns().monitors;
        monitors.erase(std::remove(monitors.begin(), monitors.end(), this), monitors.end());
        m_onChange = {};
    }
    void changed()
    {
        if (m_onChange) {
            m_onChange();
        }
    }

private:
    std::function<void()> m_onChange;
};

}  // namespace

bool removableMediaIgnored()
{
    const char *value = std::getenv("SEABASS_IGNORE_REMOVABLE_MEDIA");
    return value != nullptr && *value != '\0' && std::string_view(value) != "0";
}

std::unique_ptr<application::RemovableMediaLocator> createRemovableMediaLocator()
{
    if (removableMediaIgnored()) {
        return std::make_unique<NoRemovableMediaLocator>();
    }
#if defined(_WIN32)
    return std::make_unique<WindowsRemovableMediaLocator>();
#elif defined(__APPLE__)
    return std::make_unique<MacRemovableMediaLocator>();
#else
    return std::make_unique<LinuxRemovableMediaLocator>();
#endif
}

void setStandInSticksForTesting(std::function<std::vector<application::DetectedStick>()> sticks)
{
    std::lock_guard<std::mutex> lock(standIns().mutex);
    standIns().sticks = std::move(sticks);
}

void announceMediaChangeForTesting()
{
    if (!removableMediaIgnored()) {
        return;
    }
    std::lock_guard<std::mutex> lock(standIns().mutex);
    for (StandInMonitor *monitor : standIns().monitors) {
        monitor->changed();
    }
}

std::unique_ptr<application::RemovableMediaMonitor> createRemovableMediaMonitor()
{
    if (removableMediaIgnored()) {
        return std::make_unique<StandInMonitor>();
    }
#if defined(_WIN32)
    return std::make_unique<WindowsRemovableMediaMonitor>();
#elif defined(__APPLE__)
    return std::make_unique<MacDiskArbitrationMonitor>();
#else
    return std::make_unique<LinuxUdevMediaMonitor>();
#endif
}

std::unique_ptr<application::RemovableMediaMounter> createRemovableMediaMounter()
{
#if defined(_WIN32)
    return std::make_unique<WindowsRemovableMediaMounter>();
#elif defined(__APPLE__)
    return std::make_unique<DiskutilMediaMounter>();
#else
    return std::make_unique<UdisksctlMediaMounter>();
#endif
}

std::unique_ptr<application::UsbFormatter> createUsbFormatter()
{
#if defined(_WIN32)
    return std::make_unique<WindowsUsbFormatter>();
#elif defined(__APPLE__)
    return std::make_unique<MacUsbFormatter>();
#else
    return std::make_unique<LinuxUsbFormatter>();
#endif
}

}  // namespace seabass::infrastructure::media
