// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/media/diskutil_media_mounter.hpp"

#include "infrastructure/media/macos_disk_description.hpp"
#include "infrastructure/process/run_command.hpp"

namespace seabass::infrastructure::media
{

using process::runCommand;

namespace
{

// diskutil is the only thing these paths are ever handed to, so accept
// nothing but a disk node -- the locator is the only source of them.
std::optional<std::string> checkedDevice(const std::string &devicePath, std::string &errorMessage)
{
    auto bsdName = bsdNameFromDevicePath(devicePath);
    if (!bsdName) {
        errorMessage = "Not a disk device: " + devicePath;
    }
    return bsdName;
}

bool runDiskutil(const std::vector<std::string> &args, const char *what, std::string &errorMessage)
{
    auto result = runCommand(args);
    if (result.exitCode != 0) {
        errorMessage = result.output.empty() ? std::string("diskutil ") + what + " failed" : result.output;
        return false;
    }
    return true;
}

}  // namespace

std::optional<std::string> DiskutilMediaMounter::mount(const std::string &devicePath, std::string &errorMessage)
{
    auto bsdName = checkedDevice(devicePath, errorMessage);
    if (!bsdName || !runDiskutil({"diskutil", "mount", "/dev/" + *bsdName}, "mount", errorMessage)) {
        return std::nullopt;
    }
    // diskutil names the volume but not where it went; DiskArbitration
    // knows, and has updated its description by the time diskutil exits.
    if (auto description = describeDisk(*bsdName); description && !description->mountPoint.empty()) {
        return description->mountPoint;
    }
    return std::string();
}

bool DiskutilMediaMounter::unmount(const std::string &devicePath, std::string &errorMessage)
{
    auto bsdName = checkedDevice(devicePath, errorMessage);
    if (!bsdName) {
        return false;
    }
    // What Finder's eject does: unmount every volume on the drive and tell
    // it the media may be removed. A partition is ejected via its disk --
    // one volume alone can't make the drive safe to unplug.
    std::string disk = *bsdName;
    if (auto description = describeDisk(*bsdName)) {
        disk = description->wholeBsdName;
    }
    return runDiskutil({"diskutil", "eject", "/dev/" + disk}, "eject", errorMessage);
}

bool DiskutilMediaMounter::release(const std::string &devicePath, std::string &errorMessage)
{
    // Only this volume, and the drive stays attached -- unlike eject above,
    // which would leave nothing for a following format to write to.
    auto bsdName = checkedDevice(devicePath, errorMessage);
    return bsdName && runDiskutil({"diskutil", "unmount", "/dev/" + *bsdName}, "unmount", errorMessage);
}

}  // namespace seabass::infrastructure::media
