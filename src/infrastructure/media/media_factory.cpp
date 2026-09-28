// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/media/media_factory.hpp"

#include <cstdlib>
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
class NoRemovableMediaLocator : public application::RemovableMediaLocator
{
public:
    std::vector<application::DetectedStick> detect() override { return {}; }
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

std::unique_ptr<application::RemovableMediaMonitor> createRemovableMediaMonitor()
{
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
