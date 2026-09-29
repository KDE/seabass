// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Finding a stick's line in /proc/mounts. The kernel escapes a space in
// the mount point as \040, so a stick called "Stick 1" was never found:
// no device node, no filesystem UUID, and its identity fell back to
// label and size.

#include <cassert>
#include <iostream>
#include <sstream>
#include <string>

#include "infrastructure/system/stick_hardware_info.hpp"

using seabass::infrastructure::system::findMountEntry;
using seabass::infrastructure::system::MountEntry;

namespace
{

MountEntry find(const std::string &mounts, const std::string &mountPoint)
{
    std::istringstream in(mounts);
    return findMountEntry(in, mountPoint);
}

const std::string kOther = "sysfs /sys sysfs rw,nosuid 0 0\n"
                           "/dev/nvme0n1p2 / ext4 rw,relatime 0 0\n";

void aLabelWithASpace()
{
    const std::string mounts = kOther + "/dev/sdb1 /media/sebas/Stick\\0401 vfat rw,nosuid,nodev 0 0\n"
                               + "/dev/sdc1 /media/sebas/MY\\040STICK exfat rw,nosuid,nodev 0 0\n";
    const MountEntry one = find(mounts, "/media/sebas/Stick 1");
    std::cout << "  Stick 1: device '" << one.device << "', type '" << one.filesystem << "'\n";
    assert(one.device == "/dev/sdb1");
    assert(one.filesystem == "vfat");
    assert(!one.rawDeviceMatch);  // the earlier lookup missed it
    const MountEntry mine = find(mounts, "/media/sebas/MY STICK");
    std::cout << "  MY STICK: device '" << mine.device << "', type '" << mine.filesystem << "'\n";
    assert(mine.device == "/dev/sdc1");
    assert(mine.filesystem == "exfat");
    assert(find(mounts, "/media/sebas/Stick").device.empty());
}

void aTrailingSlash()
{
    const std::string mounts = kOther + "/dev/sdb1 /media/sebas/RV2 vfat rw 0 0\n";
    const MountEntry entry = find(mounts, "/media/sebas/RV2/");
    std::cout << "  RV2/: device '" << entry.device << "'\n";
    assert(entry.device == "/dev/sdb1");
    assert(entry.filesystem == "vfat");
    assert(!entry.rawDeviceMatch);
    assert(find(mounts, "/media/sebas/RV2").rawDeviceMatch);
    assert(find(mounts, "/").device == "/dev/nvme0n1p2");
}

void everyEscape()
{
    const std::string mounts = kOther + "/dev/sdd1 /media/sebas/a\\011b\\012c\\134d\\040e vfat rw 0 0\n";
    const MountEntry entry = find(mounts, "/media/sebas/a\tb\nc\\d e");
    std::cout << "  tab, newline, backslash, space: device '" << entry.device << "'\n";
    assert(entry.device == "/dev/sdd1");
    assert(entry.filesystem == "vfat");
    // The escaped text itself is not the mount point.
    assert(find(mounts, "/media/sebas/a\\011b\\012c\\134d\\040e").device.empty());
}

void autofsBeforeTheDevice()
{
    const std::string mounts = kOther + "systemd-1 /mnt/x autofs rw,relatime,fd=48 0 0\n"
                               + "/dev/sdb1 /mnt/x vfat rw,relatime 0 0\n";
    const MountEntry entry = find(mounts, "/mnt/x");
    std::cout << "  autofs then /dev: device '" << entry.device << "', type '" << entry.filesystem << "'\n";
    assert(entry.device == "/dev/sdb1");
    assert(entry.filesystem == "vfat");
    assert(entry.rawDeviceMatch);
    // Only the placeholder: its type, no device.
    const MountEntry pending = find(kOther + "systemd-1 /mnt/z autofs rw 0 0\n", "/mnt/z");
    assert(pending.device.empty() && pending.filesystem == "autofs" && !pending.rawDeviceMatch);
}

void theLastDeviceMountedThere()
{
    const std::string mounts = kOther + "/dev/sdb1 /mnt/y vfat rw 0 0\n" + "/dev/sdc1 /mnt/y exfat rw 0 0\n";
    const MountEntry entry = find(mounts, "/mnt/y");
    std::cout << "  two devices on one mount point: device '" << entry.device << "'\n";
    assert(entry.device == "/dev/sdc1");
    assert(entry.filesystem == "exfat");
}

void notMounted()
{
    const MountEntry entry = find(kOther, "/media/sebas/NOPE");
    assert(entry.device.empty() && entry.filesystem.empty());
    std::cout << "  an unknown mount point finds nothing\n";
}

}  // namespace

int main(int argc, char **argv)
{
    struct Case
    {
        const char *name;
        void (*run)();
    };
    const Case cases[] = {
        {"space", aLabelWithASpace},
        {"trailing-slash", aTrailingSlash},
        {"escapes", everyEscape},
        {"autofs", autofsBeforeTheDevice},
        {"last-device", theLastDeviceMountedThere},
        {"not-mounted", notMounted},
    };
    // One case by name, for seeing each fail on its own.
    const std::string only = argc > 1 ? argv[1] : "";
    for (const Case &c : cases) {
        if (only.empty() || only == c.name) {
            std::cout << c.name << "\n";
            c.run();
        }
    }
    std::cout << "stick_hardware_info_test: ok\n";
    return 0;
}
