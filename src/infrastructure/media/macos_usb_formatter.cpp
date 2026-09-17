// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/media/macos_usb_formatter.hpp"

#include "infrastructure/media/macos_disk_description.hpp"
#include "infrastructure/process/run_command.hpp"

namespace seabass::infrastructure::media
{

using process::runCommand;

std::optional<std::uint64_t> MacUsbFormatter::maxSizeFor(domain::UsbFilesystem) const
{
    // diskutil's newfs_msdos has no 32GB FAT32 ceiling, unlike Windows'
    // Format-Volume.
    return std::nullopt;
}

bool MacUsbFormatter::format(const std::string &wholeDiskPath, domain::UsbFilesystem fsType,
                             const std::string &volumeLabel, std::string &errorMessage,
                             application::ProgressReporter &progress)
{
    // FormatUsbStick has already matched wholeDiskPath against a fresh
    // detect(). This checks again, against the drive itself, that it is a
    // whole removable disk: eraseDisk on anything else destroys data this
    // project has no business touching.
    auto bsdName = bsdNameFromDevicePath(wholeDiskPath);
    auto description = bsdName ? describeDisk(*bsdName) : std::nullopt;
    if (!description || !isRemovableStickDisk(*description)) {
        errorMessage = "Refusing to format " + wholeDiskPath + ": not a removable USB or SD disk";
        return false;
    }

    progress.start("Formatting " + wholeDiskPath, 0);

    // diskutil requires a name; an empty one gets macOS's own default.
    std::string label = volumeLabel;
    if (label.empty()) {
        label = "UNTITLED";
    }
    // MBR, never GPT: CDJ/XDJ/Engine OS hardware refuses GPT sticks.
    auto result = runCommand({
        "diskutil",
        "eraseDisk",
        fsType == domain::UsbFilesystem::Fat32 ? "FAT32" : "ExFAT",
        label,
        "MBRFormat",
        "/dev/" + *bsdName,
    });
    progress.finish();
    if (result.exitCode != 0) {
        errorMessage = result.output.empty() ? "diskutil eraseDisk failed" : result.output;
        return false;
    }
    return true;
}

}  // namespace seabass::infrastructure::media
