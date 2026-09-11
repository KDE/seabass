// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/usb_filesystem.hpp"

namespace seabass::domain
{

std::string usbFilesystemName(UsbFilesystem fs)
{
    switch (fs) {
        case UsbFilesystem::Fat32:
            return "FAT32";
        case UsbFilesystem::ExFat:
            return "exFAT";
    }
    return "unknown";
}

UsbFilesystem recommendedUsbFilesystem(std::uint64_t capacityBytes)
{
    constexpr std::uint64_t Threshold = 32ULL * 1024 * 1024 * 1024;
    return capacityBytes <= Threshold ? UsbFilesystem::Fat32 : UsbFilesystem::ExFat;
}

}  // namespace seabass::domain
