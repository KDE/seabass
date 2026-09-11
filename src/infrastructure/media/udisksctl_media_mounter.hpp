// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "application/ports/removable_media_mounter.hpp"

namespace seabass::infrastructure::media
{

// Mounts/unmounts removable devices via the "udisksctl" command-line tool
// (part of udisks2), which handles polkit authorization for the active
// desktop session without requiring root.
class UdisksctlMediaMounter : public application::RemovableMediaMounter
{
public:
    std::optional<std::string> mount(const std::string &devicePath, std::string &errorMessage) override;
    bool unmount(const std::string &devicePath, std::string &errorMessage) override;
    bool release(const std::string &devicePath, std::string &errorMessage) override;
};

}  // namespace seabass::infrastructure::media
