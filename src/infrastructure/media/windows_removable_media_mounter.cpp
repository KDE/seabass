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
// After windows.h, which they need.
#include <cfgmgr32.h>
#include <setupapi.h>

#include <optional>
#include <string>
#include <vector>

#include "infrastructure/paths/utf8_path.hpp"

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
    // Fixed too: an external drive that calls itself a fixed disk (see
    // WindowsRemovableMediaLocator::detect) is mounted the same way.
    const UINT driveType = ::GetDriveTypeW(pathFromUtf8(devicePath).c_str());
    if (driveType != DRIVE_REMOVABLE && driveType != DRIVE_FIXED) {
        errorMessage = "Drive " + devicePath +
                        " is not available. If it was just ejected, reinsert the stick: "
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

// GUID_DEVINTERFACE_DISK, spelled out: the header only declares it
// unless <initguid.h> comes first, and then every file including it
// would define it once more.
constexpr GUID DiskInterface = {0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

// The number of the disk an open volume is on ("\\.\PhysicalDriveN").
std::optional<DWORD> diskNumberOf(HANDLE volume)
{
    STORAGE_DEVICE_NUMBER number = {};
    DWORD bytesReturned = 0;
    if (!::DeviceIoControl(volume, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &number, sizeof(number),
                           &bytesReturned, nullptr)) {
        return std::nullopt;
    }
    return number.DeviceNumber;
}

// Whether an open volume's disk is on the USB bus. Only then is the whole
// device stopped (requestDeviceEject): above a USB disk sits the USB
// device, above any other the controller every disk on it shares.
bool onUsbBus(HANDLE volume)
{
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    std::vector<char> buffer(1024, '\0');
    DWORD bytesReturned = 0;
    if (!::DeviceIoControl(volume, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer.data(),
                           static_cast<DWORD>(buffer.size()), &bytesReturned, nullptr)
        || bytesReturned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        return false;
    }
    return reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(buffer.data())->BusType == BusTypeUsb;
}

// Asks Windows to stop the device disk `diskNumber` is on, as "Safely
// Remove Hardware" does. For a drive that calls itself a fixed disk:
// many external USB SSDs and hard drives do, and turn down
// IOCTL_STORAGE_EJECT_MEDIA, having no media of their own to eject.
// Tried on the disk, then on the device it hangs off (the USB device):
// which of the two takes the request differs by driver. A program with
// a file open on the drive vetoes it, and nothing is stopped then.
bool requestDeviceEject(DWORD diskNumber)
{
    HDEVINFO devices =
        ::SetupDiGetClassDevsW(&DiskInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ejected = false;
    SP_DEVICE_INTERFACE_DATA interfaceData = {};
    interfaceData.cbSize = sizeof(interfaceData);
    for (DWORD index = 0; ::SetupDiEnumDeviceInterfaces(devices, nullptr, &DiskInterface, index, &interfaceData);
         ++index) {
        DWORD needed = 0;
        ::SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0, &needed, nullptr);
        if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }
        std::vector<unsigned char> buffer(needed);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA device = {};
        device.cbSize = sizeof(device);
        if (!::SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail, needed, nullptr, &device)) {
            continue;
        }
        HANDLE disk = ::CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    0, nullptr);
        if (disk == INVALID_HANDLE_VALUE) {
            continue;
        }
        const std::optional<DWORD> number = diskNumberOf(disk);
        ::CloseHandle(disk);
        if (!number || *number != diskNumber) {
            continue;
        }
        DEVINST target = device.DevInst;
        for (int attempt = 0; attempt < 2 && !ejected; ++attempt) {
            // A veto buffer given: Windows says why instead of showing
            // its own dialog.
            PNP_VETO_TYPE veto = PNP_VetoTypeUnknown;
            wchar_t vetoName[MAX_PATH] = {};
            const CONFIGRET result = ::CM_Request_Device_EjectW(target, &veto, vetoName, MAX_PATH, 0);
            if (result == CR_SUCCESS && veto == PNP_VetoTypeUnknown) {
                ejected = true;
            } else if (result == CR_REMOVE_VETOED || veto != PNP_VetoTypeUnknown) {
                // Something holds the drive: that is the answer, and the
                // device above it is not asked to go instead.
                break;
            } else if (::CM_Get_Parent(&target, target, 0) != CR_SUCCESS) {
                break;
            }
        }
        break;
    }
    ::SetupDiDestroyDeviceInfoList(devices);
    return ejected;
}

}  // namespace

bool WindowsRemovableMediaMounter::unmount(const std::string &devicePath, std::string &errorMessage)
{
    std::string volumePath = toVolumeDevicePath(devicePath);
    if (volumePath.empty()) {
        errorMessage = "No device path given.";
        return false;
    }

    HANDLE handle = ::CreateFileW(pathFromUtf8(volumePath).c_str(), GENERIC_READ | GENERIC_WRITE,
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
    const bool dismounted = lockAndDismount(handle, lastError);
    bool ok = dismounted
        && ::DeviceIoControl(handle, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
    lastError = ::GetLastError();
    // Dismounted, but the drive has no media to eject: one that calls
    // itself a fixed disk. The whole device is stopped instead, after the
    // volume handle is closed, which would otherwise veto that.
    const std::optional<DWORD> diskNumber =
        dismounted && !ok && onUsbBus(handle) ? diskNumberOf(handle) : std::nullopt;
    ::CloseHandle(handle);
    if (diskNumber && requestDeviceEject(*diskNumber)) {
        ok = true;
    }

    if (!ok && diskNumber) {
        // The media eject's error code would mislead here: the drive
        // has no media to eject, and what failed is stopping the device.
        errorMessage = "Could not eject " + devicePath
            + ". Windows would not stop the drive; a program may still be using it.";
        return false;
    }
    if (!ok) {
        errorMessage = "Could not eject " + devicePath +
                        ". It may still be in use (error " + std::to_string(lastError) + ").";
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

    HANDLE handle = ::CreateFileW(pathFromUtf8(volumePath).c_str(), GENERIC_READ | GENERIC_WRITE,
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
                        ". It may still be in use (error " + std::to_string(lastError) + ").";
        return false;
    }
    return true;
}

}  // namespace seabass::infrastructure::media
