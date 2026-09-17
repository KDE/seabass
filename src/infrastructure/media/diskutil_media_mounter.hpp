// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "application/ports/removable_media_mounter.hpp"

namespace seabass::infrastructure::media
{

// Mounts, unmounts and ejects removable devices via macOS's "diskutil",
// which needs no administrator rights for removable media belonging to
// the logged-in user's session.
class DiskutilMediaMounter : public application::RemovableMediaMounter
{
public:
    std::optional<std::string> mount(const std::string &devicePath, std::string &errorMessage) override;
    bool unmount(const std::string &devicePath, std::string &errorMessage) override;
    bool release(const std::string &devicePath, std::string &errorMessage) override;
};

}  // namespace seabass::infrastructure::media
