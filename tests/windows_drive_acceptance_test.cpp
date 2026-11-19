// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Which lettered drive Windows lists as a stick, and which of them is
// asked about before it is erased. Plain facts in, so it runs on every
// platform and needs no drive: the case from the field is a 256 GB
// Kingston USB drive that calls itself a fixed disk, which Seabass 1.0.1
// listed without its letter and could not open.

#include <cassert>
#include <iostream>

#include "infrastructure/media/windows_drive_acceptance.hpp"

using namespace seabass::infrastructure::media;

namespace
{

WindowsLetteredDrive drive(WindowsDriveKind kind, WindowsDiskBus bus, bool hotpluggable = false,
                           bool onSystemDisk = false)
{
    WindowsLetteredDrive d;
    d.kind = kind;
    d.bus = bus;
    d.hotpluggable = hotpluggable;
    d.onSystemDisk = onSystemDisk;
    return d;
}

}  // namespace

int main()
{
    using Kind = WindowsDriveKind;
    using Bus = WindowsDiskBus;
    using Accept = WindowsDriveAcceptance;

    // An ordinary stick: taken, and not asked about.
    {
        const auto stick = drive(Kind::Removable, Bus::Usb, true);
        assert(acceptWindowsDrive(stick) == Accept::Yes);
        assert(!windowsDriveReportedFixed(stick));
        std::cout << "case 1 (a removable USB stick is taken, no extra question) OK\n";
    }

    // The Kingston: fixed, on USB. Taken with or without a library, and
    // asked about before anything erases it.
    {
        const auto kingston = drive(Kind::Fixed, Bus::Usb, true);
        assert(acceptWindowsDrive(kingston) == Accept::Yes);
        assert(windowsDriveReportedFixed(kingston));
        // Whatever the hotplug answer: some bridges do not give one.
        assert(acceptWindowsDrive(drive(Kind::Fixed, Bus::Usb, false)) == Accept::Yes);
        std::cout << "case 2 (a fixed drive on USB is taken, and asked about) OK\n";
    }

    // An internal disk: never, and above all not when its SATA port is
    // reported hot-pluggable, as it is on many desktops.
    {
        assert(acceptWindowsDrive(drive(Kind::Fixed, Bus::Other, false)) == Accept::No);
        assert(acceptWindowsDrive(drive(Kind::Fixed, Bus::Other, true)) == Accept::No);
        // An internal NVMe disk is not hot-pluggable.
        assert(acceptWindowsDrive(drive(Kind::Fixed, Bus::ExternalOnly, false)) == Accept::No);
        std::cout << "case 3 (an internal disk is never taken, hot-pluggable SATA included) OK\n";
    }

    // An external drive off the USB bus (Thunderbolt enclosure, FireWire):
    // only for the library on it, and asked about.
    {
        const auto enclosure = drive(Kind::Fixed, Bus::ExternalOnly, true);
        assert(acceptWindowsDrive(enclosure) == Accept::OnlyWithLibrary);
        assert(windowsDriveReportedFixed(enclosure));
        std::cout << "case 4 (an external drive off USB is taken only with a library) OK\n";
    }

    // The disk Windows runs from: never, whatever it looks like. A
    // machine booted from a USB drive has it removable-looking and on USB.
    {
        assert(acceptWindowsDrive(drive(Kind::Fixed, Bus::Usb, true, true)) == Accept::No);
        assert(acceptWindowsDrive(drive(Kind::Removable, Bus::Usb, true, true)) == Accept::No);
        assert(acceptWindowsDrive(drive(Kind::Fixed, Bus::ExternalOnly, true, true)) == Accept::No);
        std::cout << "case 5 (the disk Windows runs from is never taken) OK\n";
    }

    // Network, optical and the rest: never.
    {
        assert(acceptWindowsDrive(drive(Kind::Other, Bus::Usb, true)) == Accept::No);
        assert(acceptWindowsDrive(drive(Kind::Other, Bus::Other)) == Accept::No);
        std::cout << "case 6 (other drive kinds are never taken) OK\n";
    }

    // A card in a built-in reader reports removable and is not on USB:
    // taken as before, and not asked about.
    {
        const auto card = drive(Kind::Removable, Bus::Other, true);
        assert(acceptWindowsDrive(card) == Accept::Yes);
        assert(!windowsDriveReportedFixed(card));
        std::cout << "case 7 (a removable drive off USB is taken as before) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
