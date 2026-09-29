// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Finding a stick's line in /proc/mounts by the mount point a page holds.
//
// The kernel writes a space in a mount point as \040 (a tab \011, a newline
// \012, a backslash \134), so a stick labelled "MY STICK" is listed under
// /media/sebas/MY\040STICK. Matched against the plain path, it was never
// found: no filesystem type and no identity for any stick with a space in
// its label.

#include <cassert>
#include <iostream>
#include <sstream>

#include "infrastructure/system/stick_hardware_info.hpp"

using seabass::infrastructure::system::findProcMount;

int main()
{
    const std::string table = "sysfs /sys sysfs rw,nosuid 0 0\n"
                              "/dev/sda2 / ext4 rw,relatime 0 0\n"
                              "/dev/sdb1 /media/sebas/MY\\040STICK vfat rw,nosuid,nodev 0 0\n"
                              "/dev/sdc1 /media/sebas/TAB\\011AND\\134SLASH\\012 exfat rw 0 0\n";
    {
        std::istringstream in(table);
        const auto entry = findProcMount(in, "/media/sebas/MY STICK");
        assert(entry && entry->device == "/dev/sdb1" && entry->filesystem == "vfat");
    }
    {
        std::istringstream in(table);
        const auto entry = findProcMount(in, "/media/sebas/MY STICK/");  // with a trailing slash
        assert(entry && entry->device == "/dev/sdb1");
    }
    {
        std::istringstream in(table);
        const auto entry = findProcMount(in, "/media/sebas/TAB\tAND\\SLASH\n");
        assert(entry && entry->device == "/dev/sdc1" && entry->filesystem == "exfat");
    }
    {
        std::istringstream in(table);
        assert(!findProcMount(in, "/media/sebas/OTHER"));
    }
    {
        std::istringstream in(table);
        const auto root = findProcMount(in, "/");
        assert(root && root->device == "/dev/sda2");
    }
    std::cout << "proc_mounts_test: all cases passed\n";
    return 0;
}
