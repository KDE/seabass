// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

#include "application/ports/removable_media_locator.hpp"

namespace seabass::infrastructure::media
{

// Finds candidate USB sticks and SD cards via IOKit and DiskArbitration:
// every partition of a removable physical drive is reported (a drive with
// no partitions at all as itself), mounted or not, with the on-disk
// signature of either format checked for on any that are mounted. The
// same shape as LinuxRemovableMediaLocator.
class MacRemovableMediaLocator : public application::RemovableMediaLocator
{
public:
    std::vector<application::DetectedStick> detect() override;
};

}  // namespace seabass::infrastructure::media
