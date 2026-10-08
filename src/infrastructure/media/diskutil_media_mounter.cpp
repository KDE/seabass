// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/media/diskutil_media_mounter.hpp"

#include "infrastructure/media/macos_disk_description.hpp"
#include "infrastructure/process/run_command.hpp"
#include "infrastructure/system/rekordbox_process_detector.hpp"

#include <regex>

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

DiskutilRefusal describeDiskutilRefusal(const std::string &rawOutput)
{
    DiskutilRefusal refusal;
    refusal.message = rawOutput;
    static const std::regex dissent(R"(dissented by PID (\d+) \(([^)]*)\))");
    std::smatch match;
    if (!std::regex_search(rawOutput, match, dissent)) {
        return refusal;
    }
    refusal.pid = std::stol(match[1].str());
    const std::string path = match[2].str();
    const std::string executable = path.substr(path.find_last_of('/') + 1);
    const std::string djSoftware = system::djSoftwareForProcessName(executable);
    refusal.holder = djSoftware.empty() ? executable : djSoftware;
    refusal.lasting = !djSoftware.empty();
    refusal.message = refusal.lasting
        ? refusal.holder + " has files on this stick open. Quit " + refusal.holder + ", then eject again."
        : refusal.holder + " (PID " + std::to_string(refusal.pid)
              + ") still has files on this stick open. Close it, then eject again.";
    return refusal;
}

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
    m_lastRefusalLasting = false;
    m_lastRefusalHolder.clear();
    if (runDiskutil({"diskutil", "eject", "/dev/" + disk}, "eject", errorMessage)) {
        return true;
    }
    const DiskutilRefusal refusal = describeDiskutilRefusal(errorMessage);
    errorMessage = refusal.message;
    m_lastRefusalLasting = refusal.lasting;
    m_lastRefusalHolder = refusal.holder;
    return false;
}

bool DiskutilMediaMounter::release(const std::string &devicePath, std::string &errorMessage)
{
    // Only this volume, and the drive stays attached -- unlike eject above,
    // which would leave nothing for a following format to write to.
    auto bsdName = checkedDevice(devicePath, errorMessage);
    return bsdName && runDiskutil({"diskutil", "unmount", "/dev/" + *bsdName}, "unmount", errorMessage);
}

}  // namespace seabass::infrastructure::media
