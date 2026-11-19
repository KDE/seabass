// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

namespace seabass::infrastructure::media
{

// Which lettered drive on Windows is listed as a stick, decided from
// plain facts so that it can be tested on any platform and without the
// hardware (windows_drive_acceptance_test): WindowsRemovableMediaLocator
// gathers the facts and asks here.

// What GetDriveType says of the letter.
enum class WindowsDriveKind {
    Removable,  // DRIVE_REMOVABLE: a stick, a card
    Fixed,      // DRIVE_FIXED: an internal disk, and many external SSDs and hard drives
    Other,      // network, optical, RAM disk, unknown
};

// The bus the letter's disk is on, as far as it matters here.
enum class WindowsDiskBus {
    Usb,
    // Buses only external or card devices use: FireWire, SD, MMC, and
    // NVMe (which, hot-pluggable, is a Thunderbolt or USB4 enclosure).
    ExternalOnly,
    // SATA, ATA, RAID, storage spaces, virtual disks, unknown. Never
    // taken, hot-pluggable or not: the SATA drivers report internal ports
    // as hot-pluggable wherever the firmware allows it.
    Other,
};

struct WindowsLetteredDrive
{
    WindowsDriveKind kind = WindowsDriveKind::Other;
    // On the disk Windows runs from (any of its letters).
    bool onSystemDisk = false;
    WindowsDiskBus bus = WindowsDiskBus::Other;
    // The device can be unplugged while the machine runs, or holds
    // removable media (IOCTL_STORAGE_GET_HOTPLUG_INFO).
    bool hotpluggable = false;
};

enum class WindowsDriveAcceptance {
    No,
    Yes,
    // Listed only if a rekordbox or Engine library is found on it: that
    // is then a drive someone brought to play from.
    OnlyWithLibrary,
};

inline WindowsDriveAcceptance acceptWindowsDrive(const WindowsLetteredDrive &drive)
{
    // Whatever else it is, the disk Windows runs from is not a stick: a
    // machine booted from a USB drive has it on the USB bus.
    if (drive.onSystemDisk) {
        return WindowsDriveAcceptance::No;
    }
    switch (drive.kind) {
    case WindowsDriveKind::Removable:
        return WindowsDriveAcceptance::Yes;
    case WindowsDriveKind::Fixed:
        // Many external USB SSDs and hard drives call themselves fixed
        // (a 256 GB Kingston did, and could not be opened in 1.0.1). On
        // the USB bus it is taken, as on Linux and macOS.
        if (drive.bus == WindowsDiskBus::Usb) {
            return WindowsDriveAcceptance::Yes;
        }
        if (drive.bus == WindowsDiskBus::ExternalOnly && drive.hotpluggable) {
            return WindowsDriveAcceptance::OnlyWithLibrary;
        }
        return WindowsDriveAcceptance::No;
    case WindowsDriveKind::Other:
        return WindowsDriveAcceptance::No;
    }
    return WindowsDriveAcceptance::No;
}

// Whether an accepted drive is one Seabass is not sure is a stick
// (DetectedStick::reportedFixed): everything that erases or overwrites it
// asks the user first.
inline bool windowsDriveReportedFixed(const WindowsLetteredDrive &drive)
{
    return drive.kind != WindowsDriveKind::Removable;
}

}  // namespace seabass::infrastructure::media
