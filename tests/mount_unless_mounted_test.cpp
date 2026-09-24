// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// mountUnlessMounted(): a stick the system mounted is never claimed as
// Seabass's own, so Seabass never ejects it on quit. Found on macOS: a
// stick plugged in while Seabass ran was counted as mounted by Seabass
// (its automatic mount raced the system's, and diskutil said yes either
// way), and quitting Seabass ejected it. The race itself is closed by not
// auto-mounting on macOS at all; this is the check every mount makes.

#include <cassert>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "application/mount_unless_mounted.hpp"

using seabass::application::DetectedStick;
using seabass::application::mountUnlessMounted;

namespace
{

// A locator whose stick is mounted by "the system" from a given look on,
// or never; the stick can also be gone.
struct FakeLocator : seabass::application::RemovableMediaLocator
{
    int looks = 0;
    int mountedFromLook = -1;  // -1: the system never mounts it
    bool present = true;
    bool mountedByMounter = false;
    std::vector<DetectedStick> detect() override
    {
        ++looks;
        if (!present) {
            return {};
        }
        DetectedStick stick;
        stick.devicePath = "/dev/disk4s1";
        stick.mounted = mountedByMounter || (mountedFromLook >= 0 && looks >= mountedFromLook);
        return {stick};
    }
};

struct FakeMounter : seabass::application::RemovableMediaMounter
{
    FakeLocator *locator = nullptr;
    int mounts = 0;
    bool fail = false;
    std::optional<std::string> mount(const std::string &, std::string &error) override
    {
        ++mounts;
        if (fail) {
            error = "no";
            return std::nullopt;
        }
        locator->mountedByMounter = true;
        return std::string("/Volumes/SANDISK_1");
    }
    bool unmount(const std::string &, std::string &) override { return true; }
    bool release(const std::string &, std::string &) override { return true; }
};

struct Rig
{
    FakeLocator locator;
    FakeMounter mounter;
    Rig() { mounter.locator = &locator; }
    seabass::application::MountOutcome run() { return mountUnlessMounted(locator, mounter, "/dev/disk4s1"); }
};

}  // namespace

int main()
{
    // 1. Already mounted (the system got there before the task ran): a
    //    success, and not ours. Nothing is mounted a second time.
    {
        Rig rig;
        rig.locator.mountedFromLook = 1;
        auto outcome = rig.run();
        assert(outcome.success);
        assert(!outcome.mountedHere);
        assert(rig.mounter.mounts == 0);
    }

    // 2. Not mounted by anyone: Seabass mounts it, and it IS ours.
    {
        Rig rig;
        auto outcome = rig.run();
        assert(outcome.success && outcome.mountedHere);
        assert(rig.mounter.mounts == 1);
    }

    // 3. The mount itself fails: not a success, not ours, the error kept.
    {
        Rig rig;
        rig.mounter.fail = true;
        auto outcome = rig.run();
        assert(!outcome.success && !outcome.mountedHere);
        assert(outcome.errorMessage == "no");
    }

    // 4. Gone by the time the task runs: no mount attempted.
    {
        Rig rig;
        rig.locator.present = false;
        auto outcome = rig.run();
        assert(!outcome.success && !outcome.mountedHere);
        assert(rig.mounter.mounts == 0);
    }

    std::cout << "mount_unless_mounted_test: all passed\n";
    return 0;
}
