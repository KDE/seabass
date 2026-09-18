// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// One value, and the reason it is worth a test of its own: a stick this
// project formatted FAT32 on Linux came back with a real mkfs.fat filesystem
// inside a partition typed 0x83, Linux. Linux never noticed -- it probes the
// content and ignores the byte. macOS trusts the byte, found a filesystem it
// has no driver for, and offered to initialize the stick, which for a DJ who
// says yes is the library.
//
// udisks2 was asked to choose the type ('') on the documented understanding
// that it picks a sensible one. It does not, and no test could have caught
// that, because the wrong answer only appears on a second operating system.
// What a test can do is pin the value now that it is known.
#include <cassert>
#include <iostream>

#include "domain/usb_filesystem.hpp"
#include "infrastructure/media/linux_usb_formatter.hpp"

using seabass::domain::UsbFilesystem;
using seabass::infrastructure::media::mbrPartitionType;

int main()
{
    // 0x0c is W95 FAT32 (LBA) -- what diskutil's own MBRFormat FAT32 writes
    // on macOS, and what a CDJ expects to find.
    assert(mbrPartitionType(UsbFilesystem::Fat32) == "0x0c");

    // 0x07 is the shared NTFS/exFAT/IFS type, as the spec intends.
    assert(mbrPartitionType(UsbFilesystem::ExFat) == "0x07");

    // Never empty: an empty type is what let udisks2 choose, and it chose
    // wrong. Never 0x83 either, which is what it chose.
    for (auto filesystem : {UsbFilesystem::Fat32, UsbFilesystem::ExFat}) {
        const std::string type = mbrPartitionType(filesystem);
        assert(!type.empty());
        assert(type != "0x83");
        assert(type.rfind("0x", 0) == 0);  // udisks2 wants it hex-prefixed
    }

    std::cout << "linux_usb_formatter_test passed\n";
    return 0;
}
