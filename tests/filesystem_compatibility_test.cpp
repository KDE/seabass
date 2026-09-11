// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>

#include "domain/filesystem_compatibility.hpp"

using namespace seabass::domain;

int main()
{
    // Case 1: vfat/FAT32 is recommended, with the real 4 GiB caveat.
    {
        auto info = FilesystemCompatibility::lookup("vfat");
        assert(info.displayName == "FAT32");
        assert(info.recommendedForDjHardware);
        assert(info.maxFileSize.find("4 Gi") != std::string::npos);
        std::cout << "case 1 (vfat recommended, 4 GiB cap noted) OK\n";
    }

    // Case 2: exFAT recommended but with a hedge about older hardware.
    {
        auto info = FilesystemCompatibility::lookup("exfat");
        assert(info.displayName == "exFAT");
        assert(info.recommendedForDjHardware);
        std::cout << "case 2 (exfat recommended) OK\n";
    }

    // Case 3: NTFS is explicitly not recommended for DJ hardware.
    {
        auto info = FilesystemCompatibility::lookup("NTFS");
        assert(!info.recommendedForDjHardware);
        std::cout << "case 3 (ntfs not recommended, case-insensitive lookup) OK\n";
    }

    // Case 4: an unknown filesystem gets a generic, honest fallback
    // rather than fabricated compatibility claims.
    {
        auto info = FilesystemCompatibility::lookup("btrfs");
        assert(!info.recommendedForDjHardware);
        assert(info.displayName == "btrfs");
        std::cout << "case 4 (unknown filesystem gets honest fallback) OK\n";
    }

    std::cout << "All filesystem_compatibility tests passed.\n";
    return 0;
}
