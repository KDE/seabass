// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/media/media_factory.hpp"

#if defined(_WIN32)
#include "infrastructure/media/windows_removable_media_locator.hpp"
#include "infrastructure/media/windows_removable_media_monitor.hpp"
#include "infrastructure/media/windows_removable_media_mounter.hpp"
#include "infrastructure/media/windows_usb_formatter.hpp"
#else
#include "infrastructure/media/linux_removable_media_locator.hpp"
#include "infrastructure/media/linux_udev_media_monitor.hpp"
#include "infrastructure/media/linux_usb_formatter.hpp"
#include "infrastructure/media/udisksctl_media_mounter.hpp"
#endif

namespace seabass::infrastructure::media
{

std::unique_ptr<application::RemovableMediaLocator> createRemovableMediaLocator()
{
#if defined(_WIN32)
    return std::make_unique<WindowsRemovableMediaLocator>();
#else
    return std::make_unique<LinuxRemovableMediaLocator>();
#endif
}

std::unique_ptr<application::RemovableMediaMonitor> createRemovableMediaMonitor()
{
#if defined(_WIN32)
    return std::make_unique<WindowsRemovableMediaMonitor>();
#else
    return std::make_unique<LinuxUdevMediaMonitor>();
#endif
}

std::unique_ptr<application::RemovableMediaMounter> createRemovableMediaMounter()
{
#if defined(_WIN32)
    return std::make_unique<WindowsRemovableMediaMounter>();
#else
    return std::make_unique<UdisksctlMediaMounter>();
#endif
}

std::unique_ptr<application::UsbFormatter> createUsbFormatter()
{
#if defined(_WIN32)
    return std::make_unique<WindowsUsbFormatter>();
#else
    return std::make_unique<LinuxUsbFormatter>();
#endif
}

}  // namespace seabass::infrastructure::media
