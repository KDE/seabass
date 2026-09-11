// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

#include "application/ports/removable_media_locator.hpp"

namespace seabass::infrastructure::media
{

// Finds candidate USB sticks on Windows. Unlike Linux (where a device can
// be plugged in but not yet mounted, and udev is needed to see it at all),
// Windows auto-assigns a drive letter to a removable volume as soon as
// it's inserted -- so every stick this reports is, in Windows terms,
// already "mounted." mounted is still always true and mountPoint always
// set for that reason; devicePath holds the drive letter itself (e.g.
// "D:\\") since Windows has no /dev-style device node to report instead.
class WindowsRemovableMediaLocator : public application::RemovableMediaLocator
{
public:
    std::vector<application::DetectedStick> detect() override;
};

}  // namespace seabass::infrastructure::media
