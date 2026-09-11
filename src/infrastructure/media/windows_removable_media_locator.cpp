// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/media/windows_removable_media_locator.hpp"

// windows.h must come before any other Windows header, and defines macros
// (min/max) this project never wants -- NOMINMAX suppresses those. MinGW's
// own C++ standard headers already define both, hence the #ifndef guards
// (a plain #define would just be redundant there, but MSVC doesn't
// predefine either, so the guards keep this correct for both toolchains).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "infrastructure/media/stick_root_scan.hpp"
#include "infrastructure/process/run_command.hpp"

namespace seabass::infrastructure::media
{

using application::DetectedStick;
using process::runCommand;

namespace
{

std::string driveLetterToPath(char letter)
{
    return std::string(1, letter) + ":\\";
}

// Returns the physical disk number (0, 1, 2, ...) a mounted drive letter
// lives on, via IOCTL_STORAGE_GET_DEVICE_NUMBER -- the standard, minimal
// WinAPI way to answer "which disk is this volume on" without needing to
// shell out to PowerShell a second time (Get-Partition -DriveLetter would
// answer the same question, but this needs no elevation, no process
// launch, and no text parsing).
std::optional<int> physicalDiskNumberForDriveLetter(char letter)
{
    std::string path = "\\\\.\\" + std::string(1, letter) + ":";
    HANDLE handle = ::CreateFileA(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                   nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    STORAGE_DEVICE_NUMBER deviceNumber = {};
    DWORD bytesReturned = 0;
    BOOL ok = ::DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &deviceNumber,
                                 sizeof(deviceNumber), &bytesReturned, nullptr);
    ::CloseHandle(handle);
    if (!ok) {
        return std::nullopt;
    }
    return static_cast<int>(deviceNumber.DeviceNumber);
}

struct UsbDiskInfo
{
    std::uint64_t sizeBytes = 0;
    std::string friendlyName;
    bool blank = false;  // PartitionStyle == "RAW" -- no partition table at all
};

// Every USB-attached whole disk, keyed by disk number -- includes disks
// with no partition table (invisible to the drive-letter-based loop
// below, which is why this exists: see the Format USB Stick plan's own
// "current stick detection can't see blank/unpartitioned drives" gap).
// Uses PowerShell's Storage module (Get-Disk), confirmed via real testing
// on Windows hardware to enumerate fully without elevation -- the same
// module WindowsUsbFormatter uses (elevated) for the actual format, so
// enumeration and formatting share one consistent API.
std::map<int, UsbDiskInfo> queryUsbDisks()
{
    std::map<int, UsbDiskInfo> disks;
    auto result = runCommand({
        "powershell.exe",
        "-NoProfile",
        "-NonInteractive",
        "-Command",
        "Get-Disk | Where-Object { $_.BusType -eq 'USB' } | ForEach-Object { "
        "\"{0}|{1}|{2}|{3}\" -f $_.Number, $_.Size, $_.FriendlyName, $_.PartitionStyle }",
    });
    if (result.exitCode != 0) {
        return disks;
    }

    std::istringstream lines(result.output);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string numberStr, sizeStr, friendlyName, partitionStyle;
        if (!std::getline(fields, numberStr, '|') || !std::getline(fields, sizeStr, '|') ||
            !std::getline(fields, friendlyName, '|') || !std::getline(fields, partitionStyle)) {
            continue;
        }
        try {
            int number = std::stoi(numberStr);
            UsbDiskInfo info;
            info.sizeBytes = std::stoull(sizeStr);
            info.friendlyName = friendlyName;
            info.blank = partitionStyle == "RAW";
            disks[number] = std::move(info);
        } catch (const std::exception &) {
            continue;  // a malformed line is skipped, not fatal to the rest of detection
        }
    }
    return disks;
}

std::string physicalDrivePath(int number)
{
    return "\\\\.\\PhysicalDrive" + std::to_string(number);
}

// The device's own serial number as reported by the storage stack
// (STORAGE_DEVICE_DESCRIPTOR::SerialNumberOffset via
// IOCTL_STORAGE_QUERY_PROPERTY) -- the Windows counterpart of udev's
// ID_SERIAL_SHORT, and the "exact same physical stick" evidence
// StickIdentity::isSameStick() prefers. Works on a "\\.\PhysicalDriveN"
// handle without elevation. Empty when the device reports none (some
// cheap sticks do) or the query fails. Unverified on real Windows
// hardware, like the rest of this file: written against the documented
// API only.
std::string storageSerialNumber(const std::string &devicePath)
{
    HANDLE handle = ::CreateFileA(devicePath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return {};
    }
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    std::vector<char> buffer(4096, '\0');
    DWORD bytesReturned = 0;
    BOOL ok = ::DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer.data(),
                                 static_cast<DWORD>(buffer.size() - 1), &bytesReturned, nullptr);
    ::CloseHandle(handle);
    if (!ok || bytesReturned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        return {};
    }
    const auto *descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(buffer.data());
    if (descriptor->SerialNumberOffset == 0 || descriptor->SerialNumberOffset >= bytesReturned) {
        return {};
    }
    // The buffer keeps a trailing NUL past what the driver may write, so
    // strnlen can never run off the end.
    const char *start = buffer.data() + descriptor->SerialNumberOffset;
    std::string serial(start, ::strnlen(start, buffer.size() - descriptor->SerialNumberOffset));
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    serial.erase(serial.begin(), std::find_if(serial.begin(), serial.end(), notSpace));
    serial.erase(std::find_if(serial.rbegin(), serial.rend(), notSpace).base(), serial.end());
    return serial;
}

// The volume serial number GetVolumeInformation reports, formatted the
// way Windows itself shows it ("1A2B-3C4D" in `dir`, here without the
// dash): the filesystem-level identity, reassigned by a format, same
// role as ID_FS_UUID on Linux.
std::string volumeSerialString(DWORD serial)
{
    char text[16] = {};
    std::snprintf(text, sizeof(text), "%08X", static_cast<unsigned int>(serial));
    return text;
}

}  // namespace

std::vector<DetectedStick> WindowsRemovableMediaLocator::detect()
{
    std::vector<DetectedStick> sticks;
    auto usbDisks = queryUsbDisks();
    std::set<int> claimedDiskNumbers;

    // Bit N set means drive letter 'A' + N exists -- the standard way to
    // enumerate assigned drive letters without touching SetupAPI's much
    // heavier device-enumeration API, which isn't needed here since
    // Windows already hands every removable volume a drive letter.
    DWORD driveMask = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(driveMask & (1u << i))) {
            continue;
        }
        std::string rootPath = driveLetterToPath(static_cast<char>('A' + i));
        if (::GetDriveTypeA(rootPath.c_str()) != DRIVE_REMOVABLE) {
            continue;
        }

        DetectedStick stick;
        stick.devicePath = rootPath;
        stick.mountPoint = rootPath;
        stick.mounted = true;

        // TODO(windows): stick.isSdCard is always false here -- unlike
        // Linux's udev ID_DRIVE_FLASH_SD property (see
        // LinuxRemovableMediaLocator::detect()'s own comment), there's no
        // single GetDriveTypeA()-level signal for "this is an SD card
        // reader, not a USB flash drive." Real detection would need
        // SetupAPI (walk the volume's PnP device instance up to its bus,
        // check for a card-reader/MMC-SD device class) or
        // IOCTL_STORAGE_QUERY_PROPERTY's STORAGE_ADAPTER_DESCRIPTOR::
        // BusType == BusTypeSd/BusTypeMmc -- not attempted here since it
        // can't be tested without a real Windows machine. Note the
        // *other* half of this same backlog item (hiding an empty
        // multislot card-reader slot) needs no Windows-side fix at all:
        // GetLogicalDrives() only ever reports a bit for a drive letter
        // Windows has actually assigned, and it never assigns one to an
        // empty slot in the first place, so there's nothing to filter.

        char volumeName[MAX_PATH + 1] = {};
        DWORD volumeSerial = 0;
        if (::GetVolumeInformationA(rootPath.c_str(), volumeName, sizeof(volumeName), &volumeSerial, nullptr,
                                     nullptr, nullptr, 0)) {
            stick.label = volumeName[0] != '\0' ? std::string(volumeName) : rootPath;
            if (volumeSerial != 0) {
                stick.identity.filesystemUuid = volumeSerialString(volumeSerial);
            }
        } else {
            stick.label = rootPath;
        }

        if (auto diskNumber = physicalDiskNumberForDriveLetter(static_cast<char>('A' + i))) {
            if (auto it = usbDisks.find(*diskNumber); it != usbDisks.end()) {
                stick.wholeDiskPath = physicalDrivePath(*diskNumber);
                stick.capacityBytes = it->second.sizeBytes;
                stick.hasNoFilesystem = false;  // it has a mounted volume, so it's clearly not blank
                claimedDiskNumbers.insert(*diskNumber);
                stick.identity.hardwareSerial = storageSerialNumber(stick.wholeDiskPath);
            }
        }
        stick.identity.label = stick.label;
        stick.identity.capacityBytes = stick.capacityBytes;

        scanMountedRoot(rootPath, stick);
        sticks.push_back(std::move(stick));
    }

    // Any USB disk not already represented by a mounted drive letter above
    // -- most commonly a genuinely blank drive (PartitionStyle == "RAW"
    // has no filesystem to assign a letter to at all), but also covers a
    // drive whose one partition simply isn't mounted for some other
    // reason. This is what makes a blank stick visible to the Format USB
    // Stick feature at all -- GetDriveTypeA() alone never can, since it
    // only ever answers for a path that already has a drive letter.
    for (const auto &[number, info] : usbDisks) {
        if (claimedDiskNumbers.count(number)) {
            continue;
        }
        DetectedStick stick;
        stick.wholeDiskPath = physicalDrivePath(number);
        stick.capacityBytes = info.sizeBytes;
        stick.hasNoFilesystem = info.blank;
        stick.label = info.friendlyName.empty() ? stick.wholeDiskPath : info.friendlyName;
        stick.mounted = false;
        stick.identity.hardwareSerial = storageSerialNumber(stick.wholeDiskPath);
        stick.identity.label = stick.label;
        stick.identity.capacityBytes = stick.capacityBytes;
        sticks.push_back(std::move(stick));
    }

    return sticks;
}

}  // namespace seabass::infrastructure::media
