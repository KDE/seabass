// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
#include <memory>
#include <vector>

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
// True when SEABASS_IGNORE_REMOVABLE_MEDIA is set (and not "0"): the
// locators this factory hands out then find nothing. The test suites set
// it so that no test ever reads or writes a stick plugged into this
// computer.
bool removableMediaIgnored();

// Test seams for a suite that wants sticks without seeing real ones: the
// storm (tests/qml-storm) plugs and pulls directories of its own. Both
// act only while removableMediaIgnored(), so they can add stand-ins to an
// empty list but never let a real stick into it.
//
// setStandInSticksForTesting: what the ignoring locator answers from now
// on (nullptr: nothing, as before). Called on whichever thread detects.
// announceMediaChangeForTesting: what the OS says when a stick goes in or
// out -- every monitor handed out while ignoring calls its onChange, so
// MediaController re-detects the way it does after a real hotplug.
void setStandInSticksForTesting(std::function<std::vector<application::DetectedStick>()> sticks);
void announceMediaChangeForTesting();

std::unique_ptr<application::RemovableMediaLocator> createRemovableMediaLocator();
std::unique_ptr<application::RemovableMediaMonitor> createRemovableMediaMonitor();
std::unique_ptr<application::RemovableMediaMounter> createRemovableMediaMounter();
std::unique_ptr<application::UsbFormatter> createUsbFormatter();

}  // namespace seabass::infrastructure::media
