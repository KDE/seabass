// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <memory>

#include "application/ports/removable_media_locator.hpp"
#include "application/ports/removable_media_monitor.hpp"
#include "application/ports/removable_media_mounter.hpp"
#include "application/ports/usb_formatter.hpp"

namespace seabass::infrastructure::media
{

// Single place both composition roots (cli/main.cpp, gui/media_controller.cpp)
// get a concrete removable-media adapter from -- so which OS-specific class
// backs each port is decided exactly once, not re-selected (or, worse,
// hardcoded to the Linux one) at every call site.
std::unique_ptr<application::RemovableMediaLocator> createRemovableMediaLocator();
std::unique_ptr<application::RemovableMediaMonitor> createRemovableMediaMonitor();
std::unique_ptr<application::RemovableMediaMounter> createRemovableMediaMounter();
std::unique_ptr<application::UsbFormatter> createUsbFormatter();

}  // namespace seabass::infrastructure::media
