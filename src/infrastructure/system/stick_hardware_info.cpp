#include "infrastructure/system/stick_hardware_info.hpp"

#include "domain/stick_performance.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

#if defined(__linux__)
#include <sys/statvfs.h>

#include <libudev.h>
#include <memory>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace seabass::infrastructure::system
{

namespace
{

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Classifies a raw negotiated link speed (as reported by the OS, in
// Mbps) into the human label DJs actually recognize ("USB 2.0",
// "USB 3.0")  -- see the header's own doc comment for why this speed
// class, not physical connector shape, is what gets tracked.
std::string classifySpeed(double mbps)
{
    std::ostringstream label;
    if (mbps >= 19000) {
        label << "20 Gbps (USB 3.2 Gen2x2 SuperSpeed+)";
    } else if (mbps >= 9000) {
        label << "10 Gbps (USB 3.1/3.2 Gen2 SuperSpeed+)";
    } else if (mbps >= domain::kSuperSpeedMbps) {
        label << "5 Gbps (USB 3.0/3.1 Gen1 SuperSpeed)";
    } else if (mbps >= 400) {
        label << "480 Mbps (USB 2.0 High-Speed)";
    } else if (mbps >= 10) {
        label << "12 Mbps (USB 1.1 Full-Speed)";
    } else if (mbps > 0) {
        label << "1.5 Mbps (USB 1.0 Low-Speed)";
    } else {
        return "";
    }
    return label.str();
}

std::string fallbackIdentifier(const std::string &stickLabel, std::uint64_t totalBytes)
{
    return stickLabel + "-" + std::to_string(totalBytes);
}

}  // namespace

#if defined(__linux__)

namespace
{

// Same /proc/mounts convention as
// infrastructure/media/linux_removable_media_locator.cpp's own
// readDeviceMountPoints() (kept separate rather than shared -- that one
// maps device->mountpoint for stick discovery, this needs the reverse:
// a stick page only ever has a mount point, never the device node).
std::string deviceNodeForMountPoint(const std::string &mountPoint)
{
    std::ifstream in("/proc/mounts");
    std::string device, mount, rest;
    while (in >> device >> mount) {
        std::getline(in, rest);  // consume rest of the line
        if (mount == mountPoint && device.rfind("/dev/", 0) == 0) {
            return device;
        }
    }
    return "";
}

std::string basename(const std::string &path)
{
    auto pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

}  // namespace

StickHardwareInfo readStickHardwareInfo(const std::string &mountPoint, const std::string &stickLabel)
{
    StickHardwareInfo info;

    // Filesystem type + capacity: /proc/mounts and statvfs both key off
    // the mount point directly, no device node needed.
    {
        std::ifstream in("/proc/mounts");
        std::string device, mount, fstype, rest;
        while (in >> device >> mount >> fstype) {
            std::getline(in, rest);
            if (mount == mountPoint) {
                info.filesystem = lower(fstype);
                break;
            }
        }
    }
    {
        struct statvfs vfs{};
        if (statvfs(mountPoint.c_str(), &vfs) == 0) {
            info.totalBytes = static_cast<std::uint64_t>(vfs.f_blocks) * vfs.f_frsize;
            info.freeBytes = static_cast<std::uint64_t>(vfs.f_bavail) * vfs.f_frsize;
            // On vfat/exfat the kernel reports the cluster size as the
            // fragment size; confirmed 32 KiB on a real stick.
            info.clusterBytes = vfs.f_frsize;
        }
    }

    // USB speed + a stable identifier: both come from udev, keyed off the
    // device node backing this mount point.
    std::string devnode = deviceNodeForMountPoint(mountPoint);
    if (!devnode.empty()) {
        std::unique_ptr<udev, decltype(&udev_unref)> ctx(udev_new(), udev_unref);
        if (ctx) {
            std::unique_ptr<udev_device, decltype(&udev_device_unref)> dev(
                udev_device_new_from_subsystem_sysname(ctx.get(), "block", basename(devnode).c_str()),
                udev_device_unref);
            if (dev) {
                if (const char *uuid = udev_device_get_property_value(dev.get(), "ID_FS_UUID")) {
                    info.stickIdentifier = uuid;
                }
                if (udev_device *usbDev =
                        udev_device_get_parent_with_subsystem_devtype(dev.get(), "usb", "usb_device")) {
                    if (const char *speed = udev_device_get_sysattr_value(usbDev, "speed")) {
                        double mbps = 0.0;
                        try {
                            mbps = std::stod(speed);
                        } catch (const std::exception &) {
                            mbps = 0.0;
                        }
                        info.usbSpeedMbps = mbps;
                        info.usbSpeedLabel = classifySpeed(mbps);
                    }
                }
            }
        }
    }

    if (info.stickIdentifier.empty()) {
        info.stickIdentifier = fallbackIdentifier(stickLabel, info.totalBytes);
    }

    return info;
}

#elif defined(_WIN32)

// Unverified on real Windows hardware -- written against documented
// Win32 APIs (GetVolumeInformationW, GetDiskFreeSpaceExW) but this
// project's dev/test environment is Linux-only, see the class comment
// in stick_hardware_info.hpp. USB speed detection is not implemented
// here: unlike Linux's plain sysfs "speed" attribute, doing this
// properly on Windows needs SetupAPI/IOCTL_STORAGE_QUERY_PROPERTY
// device enumeration, a meaningfully bigger and riskier-to-get-right-
// unverified undertaking than this pass covers -- usbSpeedLabel/
// usbSpeedMbps are left at their "unknown" defaults on this platform
// for now.
StickHardwareInfo readStickHardwareInfo(const std::string &mountPoint, const std::string &stickLabel)
{
    StickHardwareInfo info;

    std::wstring root(mountPoint.begin(), mountPoint.end());
    if (!root.empty() && root.back() != L'\\') {
        root += L'\\';
    }

    wchar_t fsName[MAX_PATH + 1] = {};
    DWORD volumeSerial = 0;
    if (GetVolumeInformationW(root.c_str(), nullptr, 0, &volumeSerial, nullptr, nullptr, fsName, MAX_PATH)) {
        std::wstring wname(fsName);
        info.filesystem = lower(std::string(wname.begin(), wname.end()));
    }

    ULARGE_INTEGER freeAvailable{}, totalBytes{}, totalFree{};
    if (GetDiskFreeSpaceExW(root.c_str(), &freeAvailable, &totalBytes, &totalFree)) {
        info.totalBytes = totalBytes.QuadPart;
        info.freeBytes = freeAvailable.QuadPart;
    }
    DWORD sectorsPerCluster = 0, bytesPerSector = 0, freeClusters = 0, totalClusters = 0;
    if (GetDiskFreeSpaceW(root.c_str(), &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters)) {
        info.clusterBytes = static_cast<std::uint64_t>(sectorsPerCluster) * bytesPerSector;
    }

    // The volume serial plays the role ID_FS_UUID plays on Linux --
    // formatted exactly as WindowsRemovableMediaLocator formats it
    // (8 upper-case hex digits), so StickIdentity::libraryId() and this
    // identifier agree on the same stick.
    if (volumeSerial != 0) {
        char text[16] = {};
        std::snprintf(text, sizeof(text), "%08X", static_cast<unsigned int>(volumeSerial));
        info.stickIdentifier = text;
    } else {
        info.stickIdentifier = fallbackIdentifier(stickLabel, info.totalBytes);
    }
    return info;
}

#else

StickHardwareInfo readStickHardwareInfo(const std::string & /*mountPoint*/, const std::string &stickLabel)
{
    StickHardwareInfo info;
    info.stickIdentifier = fallbackIdentifier(stickLabel, 0);
    return info;
}

#endif

}  // namespace seabass::infrastructure::system
