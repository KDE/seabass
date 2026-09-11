// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

#include "application/ports/removable_media_locator.hpp"

namespace seabass::infrastructure::media
{

// Finds candidate USB sticks via udev: every USB block device (mounted or
// not) is reported, with the on-disk signature of either format checked
// for on any that are currently mounted.
class LinuxRemovableMediaLocator : public application::RemovableMediaLocator
{
public:
    std::vector<application::DetectedStick> detect() override;
};

}  // namespace seabass::infrastructure::media
