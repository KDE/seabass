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
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "infrastructure/media/stick_root_scan.hpp"
#include "infrastructure/media/windows_drive_acceptance.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::infrastructure::media
{

using application::DetectedStick;

namespace
{

std::string driveLetterToPath(char letter)
{
    return std::string(1, letter) + ":\\";
}

// A wide string from the OS (a volume label, which the user named and
// which can hold anything) as UTF-8, the encoding every std::string in
// Seabass carries.
// Returns the physical disk number (0, 1, 2, ...) a mounted drive letter
// lives on, via IOCTL_STORAGE_GET_DEVICE_NUMBER -- the standard, minimal
// WinAPI way to answer "which disk is this volume on" without needing to
// shell out to PowerShell a second time (Get-Partition -DriveLetter would
// answer the same question, but this needs no elevation, no process
// launch, and no text parsing).
std::optional<int> physicalDiskNumberForDriveLetter(char letter)
{
    const std::string path = "\\\\.\\" + std::string(1, letter) + ":";
    HANDLE handle = ::CreateFileW(pathFromUtf8(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
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
    // What the device says of itself (STORAGE_DEVICE_DESCRIPTOR::
    // RemovableMedia): a stick says yes, an external SSD or hard drive no.
    bool removableMedia = false;
};

// A descriptor string at `offset` in an IOCTL_STORAGE_QUERY_PROPERTY
// reply, trimmed; empty when the device reports none. The buffer keeps a
// trailing NUL past what the driver may write, so strnlen cannot run off
// the end.
std::string descriptorString(const std::vector<char> &buffer, DWORD offset, DWORD bytesReturned)
{
    if (offset == 0 || offset >= bytesReturned) {
        return {};
    }
    const char *start = buffer.data() + offset;
    std::string text(start, ::strnlen(start, buffer.size() - offset));
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

// Every USB-attached whole disk that has media in it, keyed by disk
// number -- includes disks with no partition table (invisible to the
// drive-letter-based loop below, which is why this exists: see the Format
// USB Stick plan's own "current stick detection can't see
// blank/unpartitioned drives" gap).
//
// Asked of each \\.\PhysicalDriveN directly, with access 0 and so no
// elevation, through IOCTLs defined FILE_ANY_ACCESS. It used to run
// PowerShell's Get-Disk: 1.5 s per call on the shakedown laptop, and
// detect() runs on the UI thread every time the drive set changes, so
// every hotplug and every eject froze the window for as long -- the
// Eject button's stutter (issue #26).
//
// A disk with no media is left out. A stick ejected through the app
// (WindowsRemovableMediaMounter: IOCTL_STORAGE_EJECT_MEDIA) stays
// attached as a disk with no media until it is pulled, and Get-Disk
// reported it as PartitionStyle RAW, size 0 -- which read as a blank
// drive, offered for formatting.
std::map<int, UsbDiskInfo> queryUsbDisks()
{
    std::map<int, UsbDiskInfo> disks;
    for (int number = 0; number < 64; ++number) {
        const std::string path = "\\\\.\\PhysicalDrive" + std::to_string(number);
        HANDLE handle = ::CreateFileW(pathFromUtf8(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            continue;  // numbers need not be contiguous
        }
        STORAGE_PROPERTY_QUERY query = {};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;
        std::vector<char> buffer(4096, '\0');
        DWORD bytesReturned = 0;
        const BOOL described = ::DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                                                 buffer.data(), static_cast<DWORD>(buffer.size() - 1),
                                                 &bytesReturned, nullptr);
        const auto *descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(buffer.data());
        if (!described || bytesReturned < sizeof(STORAGE_DEVICE_DESCRIPTOR) || descriptor->BusType != BusTypeUsb) {
            ::CloseHandle(handle);
            continue;
        }
        DWORD ignored = 0;
        const BOOL hasMedia =
            ::DeviceIoControl(handle, IOCTL_STORAGE_CHECK_VERIFY2, nullptr, 0, nullptr, 0, &ignored, nullptr);
        DISK_GEOMETRY_EX geometry = {};
        const BOOL sized = hasMedia
            && ::DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geometry,
                                 sizeof(geometry), &ignored, nullptr);
        PARTITION_INFORMATION_EX partition = {};
        const BOOL styled = hasMedia
            && ::DeviceIoControl(handle, IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr, 0, &partition,
                                 sizeof(partition), &ignored, nullptr);
        ::CloseHandle(handle);
        if (!hasMedia || !sized || geometry.DiskSize.QuadPart <= 0) {
            continue;
        }
        UsbDiskInfo info;
        info.sizeBytes = static_cast<std::uint64_t>(geometry.DiskSize.QuadPart);
        const std::string vendor = descriptorString(buffer, descriptor->VendorIdOffset, bytesReturned);
        const std::string product = descriptorString(buffer, descriptor->ProductIdOffset, bytesReturned);
        info.friendlyName = vendor.empty() ? product : product.empty() ? vendor : vendor + " " + product;
        info.blank = styled && partition.PartitionStyle == PARTITION_STYLE_RAW;
        info.removableMedia = descriptor->RemovableMedia != FALSE;
        disks[number] = std::move(info);
    }
    return disks;
}

// The bus of the disk a drive letter lives on and whether it can be
// unplugged, for a disk that is not on the USB bus: what
// acceptWindowsDrive() decides an external drive by. Two device
// questions through the volume's handle, opened with no access, and no
// read of the filesystem, so a sleeping hard disk is not woken: an
// internal disk is turned down on this and then never looked into
// (detect() runs on the UI thread). Left as it was when the device does
// not answer, which is "no".
void describeNonUsbDisk(char letter, WindowsLetteredDrive &drive)
{
    const std::string path = "\\\\.\\" + std::string(1, letter) + ":";
    HANDLE handle = ::CreateFileW(pathFromUtf8(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    STORAGE_HOTPLUG_INFO hotplug = {};
    DWORD bytesReturned = 0;
    const BOOL hotplugKnown = ::DeviceIoControl(handle, IOCTL_STORAGE_GET_HOTPLUG_INFO, nullptr, 0, &hotplug,
                                                sizeof(hotplug), &bytesReturned, nullptr);
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    std::vector<char> buffer(4096, '\0');
    const BOOL described = ::DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                                             buffer.data(), static_cast<DWORD>(buffer.size() - 1), &bytesReturned,
                                             nullptr);
    ::CloseHandle(handle);
    if (!hotplugKnown || !described || bytesReturned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        return;
    }
    drive.hotpluggable = hotplug.DeviceHotplug || hotplug.MediaRemovable;
    const auto bus = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(buffer.data())->BusType;
    if (bus == BusTypeUsb) {
        // A USB disk queryUsbDisks() left out (a bridge that does not
        // answer its media check): still a USB drive.
        drive.bus = WindowsDiskBus::Usb;
    } else if (bus == BusType1394 || bus == BusTypeSd || bus == BusTypeMmc || bus == BusTypeNvme) {
        drive.bus = WindowsDiskBus::ExternalOnly;
    }
}

// The letter Windows itself runs from, upper case, or 0 when it cannot be
// told. Never listed: whatever is on it, it is not a stick.
char systemDriveLetter()
{
    wchar_t directory[MAX_PATH + 1] = {};
    const UINT length = ::GetWindowsDirectoryW(directory, MAX_PATH + 1);
    if (length == 0 || length > MAX_PATH || directory[1] != L':') {
        return 0;
    }
    return static_cast<char>(std::toupper(static_cast<unsigned char>(directory[0])));
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
    HANDLE handle = ::CreateFileW(pathFromUtf8(devicePath).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
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
    return descriptorString(buffer, descriptor->SerialNumberOffset, bytesReturned);
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
    // The disk Windows runs from is never listed, under any of its
    // letters or as a bare disk: on a machine booted from a USB drive it
    // is a USB disk like any other, and would be offered for formatting.
    const char systemLetter = systemDriveLetter();
    const std::optional<int> systemDisk =
        systemLetter != 0 ? physicalDiskNumberForDriveLetter(systemLetter) : std::nullopt;
    if (systemDisk) {
        usbDisks.erase(*systemDisk);
    }
    for (int i = 0; i < 26; ++i) {
        if (!(driveMask & (1u << i))) {
            continue;
        }
        const char letter = static_cast<char>('A' + i);
        const std::string rootPath = driveLetterToPath(letter);
        const std::filesystem::path rootDirectory = pathFromUtf8(rootPath);
        const UINT driveType = ::GetDriveTypeW(rootDirectory.c_str());
        if (driveType != DRIVE_REMOVABLE && driveType != DRIVE_FIXED) {
            continue;
        }
        // A drive Windows calls fixed: many external USB SSDs and hard
        // drives say that of themselves (Kingston's XS series among them),
        // and were listed only as a disk "not mounted" that could not be
        // mounted, while Windows had long given it a letter. Taken when
        // it is on the USB bus, as on Linux and macOS, or, on any other
        // bus, when it can be unplugged and holds a DJ library: that is a
        // drive someone brought to play from. Never the drive Windows runs
        // from, and never an internal disk. Marked reportedFixed either
        // way, so whatever would erase it asks first. The rule itself is
        // acceptWindowsDrive(); here its facts are gathered.
        const bool fixed = driveType == DRIVE_FIXED;
        const std::optional<int> diskNumber = physicalDiskNumberForDriveLetter(letter);
        WindowsLetteredDrive facts;
        facts.kind = fixed ? WindowsDriveKind::Fixed : WindowsDriveKind::Removable;
        facts.onSystemDisk = letter == systemLetter || (diskNumber && systemDisk && *diskNumber == *systemDisk);
        if (diskNumber && usbDisks.count(*diskNumber) > 0) {
            facts.bus = WindowsDiskBus::Usb;
        } else if (fixed && !facts.onSystemDisk) {
            describeNonUsbDisk(letter, facts);
        }
        const WindowsDriveAcceptance acceptance = acceptWindowsDrive(facts);
        if (acceptance == WindowsDriveAcceptance::No) {
            continue;
        }

        // The wide call: a volume label is the user's own text, and the
        // narrow one would hand it back through the ANSI code page.
        wchar_t volumeName[MAX_PATH + 1] = {};
        DWORD volumeSerial = 0;
        const BOOL readable = ::GetVolumeInformationW(rootDirectory.c_str(), volumeName, MAX_PATH + 1,
                                                      &volumeSerial, nullptr, nullptr, nullptr, 0);
        // A letter with no media behind it is not a stick. Windows keeps
        // the letter of a stick ejected through the app until it is
        // pulled, and it was listed as mounted, under its bare letter,
        // with no library and the tools still offered (issue #26). What
        // the eject promised -- it is safe to pull -- is that it is gone.
        if (!readable) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_NOT_READY || error == ERROR_NO_MEDIA_IN_DRIVE) {
                continue;
            }
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

        if (readable) {
            stick.label = volumeName[0] != L'\0' ? seabass::pathToUtf8(std::filesystem::path(volumeName)) : rootPath;
            if (volumeSerial != 0) {
                stick.identity.filesystemUuid = volumeSerialString(volumeSerial);
            }
        } else {
            stick.label = rootPath;
        }

        stick.reportedFixed = windowsDriveReportedFixed(facts);
        if (diskNumber) {
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
        // Off the USB bus, the library is the only reason to list it.
        if (acceptance == WindowsDriveAcceptance::OnlyWithLibrary && !stick.rekordboxPath && !stick.enginePath
            && !stick.libraryAccessDenied) {
            continue;
        }
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
        // No letter to ask Windows about, so the device's own word: an
        // external hard drive holding only another system's partitions
        // lands here, and is the likeliest disk with other data on it.
        stick.reportedFixed = !info.removableMedia;
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
