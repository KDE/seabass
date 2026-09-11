// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/media/windows_removable_media_mounter.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <string>

namespace seabass::infrastructure::media
{

namespace
{

// devicePath is a drive path like "D:\\" (see WindowsRemovableMediaLocator)
// -- DeviceIoControl needs the volume's device path instead, "\\.\D:" (no
// trailing backslash).
std::string toVolumeDevicePath(const std::string &devicePath)
{
    if (devicePath.empty()) {
        return {};
    }
    return std::string("\\\\.\\") + devicePath[0] + ":";
}

}  // namespace

std::optional<std::string> WindowsRemovableMediaMounter::mount(const std::string &devicePath,
                                                                 std::string &errorMessage)
{
    // Windows assigns a drive letter to a removable volume the moment
    // it's inserted -- there's no separate mount action to perform.
    // This just confirms the drive is actually there and ready. If it
    // isn't, the most likely cause is that it was previously ejected via
    // unmount() below: Windows won't reassign a drive letter to an
    // ejected volume without it being physically reinserted, unlike
    // udisksctl's mount on Linux, which can genuinely bring an unmounted
    // device back without a reinsertion.
    if (::GetDriveTypeA(devicePath.c_str()) != DRIVE_REMOVABLE) {
        errorMessage = "Drive " + devicePath +
                        " is not available. If it was just ejected, reinsert the stick -- "
                        "Windows can't remount an ejected drive without that.";
        return std::nullopt;
    }
    return devicePath;
}

namespace
{

// Shared lock -> dismount step used by both unmount() (which goes on to
// eject) and release() (which deliberately stops here). Locking fails
// (harmlessly refusing, same as udisksctl would) if another process still
// has a file open on the volume.
bool lockAndDismount(HANDLE handle, DWORD &lastError)
{
    DWORD bytesReturned = 0;
    bool ok = ::DeviceIoControl(handle, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &bytesReturned, nullptr) &&
              ::DeviceIoControl(handle, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
    lastError = ::GetLastError();
    return ok;
}

}  // namespace

bool WindowsRemovableMediaMounter::unmount(const std::string &devicePath, std::string &errorMessage)
{
    std::string volumePath = toVolumeDevicePath(devicePath);
    if (volumePath.empty()) {
        errorMessage = "No device path given.";
        return false;
    }

    HANDLE handle = ::CreateFileA(volumePath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        errorMessage = "Could not open " + devicePath + " (error " + std::to_string(::GetLastError()) + ").";
        return false;
    }

    DWORD lastError = 0;
    DWORD bytesReturned = 0;
    // Same lock -> dismount -> eject sequence Windows' own "Safely Remove
    // Hardware" performs. This physically ejects the media -- only use
    // this for a user-initiated eject. A caller that intends to keep
    // operating on the same disk (e.g. FormatUsbStick) must call
    // release() instead: an ejected Windows volume needs a physical
    // reinsertion before it's usable again, unlike a plain dismount.
    bool ok = lockAndDismount(handle, lastError) &&
              ::DeviceIoControl(handle, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
    lastError = ::GetLastError();
    ::CloseHandle(handle);

    if (!ok) {
        errorMessage = "Could not eject " + devicePath +
                        " -- it may still be in use (error " + std::to_string(lastError) + ").";
        return false;
    }
    return true;
}

bool WindowsRemovableMediaMounter::release(const std::string &devicePath, std::string &errorMessage)
{
    std::string volumePath = toVolumeDevicePath(devicePath);
    if (volumePath.empty()) {
        errorMessage = "No device path given.";
        return false;
    }

    HANDLE handle = ::CreateFileA(volumePath.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        errorMessage = "Could not open " + devicePath + " (error " + std::to_string(::GetLastError()) + ").";
        return false;
    }

    DWORD lastError = 0;
    bool ok = lockAndDismount(handle, lastError);
    ::CloseHandle(handle);

    if (!ok) {
        errorMessage = "Could not release " + devicePath +
                        " -- it may still be in use (error " + std::to_string(lastError) + ").";
        return false;
    }
    return true;
}

}  // namespace seabass::infrastructure::media
