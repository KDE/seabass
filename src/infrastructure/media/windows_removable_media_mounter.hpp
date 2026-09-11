// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "application/ports/removable_media_mounter.hpp"

namespace seabass::infrastructure::media
{

// Windows auto-assigns a drive letter to a removable volume as soon as
// it's inserted, so there's no separate "mount" step to perform the way
// udisksctl needs one on Linux -- mount() just confirms the drive is
// present and ready. unmount() does real work: the same
// lock-volume -> dismount-volume -> eject-media sequence Windows' own
// "Safely Remove Hardware" uses, via DeviceIoControl -- including the
// physical eject, which leaves the drive without a usable volume until
// reinserted. release() performs only the lock/dismount portion, for
// callers (FormatUsbStick) that need the filesystem released but intend
// to keep operating on the same disk.
class WindowsRemovableMediaMounter : public application::RemovableMediaMounter
{
public:
    std::optional<std::string> mount(const std::string &devicePath, std::string &errorMessage) override;
    bool unmount(const std::string &devicePath, std::string &errorMessage) override;
    bool release(const std::string &devicePath, std::string &errorMessage) override;
};

}  // namespace seabass::infrastructure::media
