// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
#include <string>

namespace seabass::application
{

// Port for mounting/unmounting a removable device by its device path (e.g.
// "/dev/sdb1"), without the caller needing to know the host mechanism
// (udisks2 on Linux, ...).
class RemovableMediaMounter
{
public:
    virtual ~RemovableMediaMounter() = default;

    // On success, returns the mount point. On failure, returns nullopt and
    // fills errorMessage with a human-readable reason.
    virtual std::optional<std::string> mount(const std::string &devicePath, std::string &errorMessage) = 0;

    // Detaches the device the way a user consciously "ejecting" it would
    // expect: safe to physically unplug afterwards. On Windows this also
    // spins the media down and signals removal via IOCTL_STORAGE_EJECT_MEDIA,
    // which leaves the drive without a usable volume until it's physically
    // reinserted -- fine for a user-initiated eject, fatal for a caller that
    // still intends to write to the same disk. Use release() for that case
    // instead.
    virtual bool unmount(const std::string &devicePath, std::string &errorMessage) = 0;

    // Releases the filesystem so the underlying disk can be safely
    // repartitioned/reformatted, without ejecting or otherwise signaling
    // physical removal. Callers that intend to keep operating on the same
    // disk afterwards (e.g. FormatUsbStick, immediately before it wipes and
    // reformats) must use this, not unmount() -- see that port method's
    // comment for why unmount() is unsafe for this purpose on Windows.
    virtual bool release(const std::string &devicePath, std::string &errorMessage) = 0;
};

}  // namespace seabass::application
