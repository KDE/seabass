#pragma once

#include <cstdint>
#include <string>

namespace seabass::infrastructure::system
{

// Best-effort hardware facts about a mounted stick, resolved from its
// mount point alone (the GUI only ever has stick root paths, never a raw
// device node). Every field defaults to "unknown" rather than throwing --
// this backs an informational statistics page, not a write path, so a
// partial answer beats a hard failure.
struct StickHardwareInfo
{
    std::string filesystem;    // lowercase OS-reported type, e.g. "vfat", "exfat", "ntfs3"; empty if undetectable
    std::uint64_t totalBytes = 0;
    std::uint64_t freeBytes = 0;  // available to this user, not raw free blocks (matches "df"'s own convention)
    // The filesystem's allocation unit (FAT cluster): a stick formatted
    // with tiny clusters has a huge allocation table and pays one USB
    // command per cluster run when reading a fragmented file. 0 if
    // unknown.
    std::uint64_t clusterBytes = 0;
    // The negotiated USB link speed, not the physical connector shape
    // (USB-A vs USB-C): the kernel exposes the former via a plain sysfs
    // attribute on the USB device, but has no portable, generic way to
    // expose the latter for an arbitrary mass-storage device -- see
    // linux_stick_hardware_info.cpp's own comment for why speed class is
    // used as the practical stand-in for "which USB generation/port."
    std::string usbSpeedLabel;  // e.g. "480 Mbps (USB 2.0 High-Speed)"; empty if not detected as a USB device
    double usbSpeedMbps = 0.0;  // 0 if unknown
    // Best-effort stable identifier for this specific physical stick
    // (filesystem UUID when available), so benchmark history can tell
    // "same stick, different session" apart from "different stick."
    // Falls back to stickLabel + total capacity when no UUID is
    // available -- never empty.
    std::string stickIdentifier;
};

StickHardwareInfo readStickHardwareInfo(const std::string &mountPoint, const std::string &stickLabel);

}  // namespace seabass::infrastructure::system
