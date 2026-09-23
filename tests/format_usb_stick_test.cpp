// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <iostream>
#include <vector>

#include "application/use_cases/format_usb_stick.hpp"

using namespace seabass::application;
using namespace seabass::domain;

namespace
{

class FakeLocator : public RemovableMediaLocator
{
public:
    std::vector<DetectedStick> disks;
    std::vector<DetectedStick> detect() override { return disks; }
};

class FakeMounter : public RemovableMediaMounter
{
public:
    std::vector<std::string> releasedPaths;
    std::vector<std::string> mountedPaths;
    bool releaseShouldFail = false;
    bool mountShouldFail = false;

    std::optional<std::string> mount(const std::string &devicePath, std::string &errorMessage) override
    {
        if (mountShouldFail) {
            errorMessage = "fake mount failure";
            return std::nullopt;
        }
        mountedPaths.push_back(devicePath);
        return "/media/fake/STICK";
    }
    bool unmount(const std::string &, std::string &) override
    {
        assert(false && "FormatUsbStick must use release(), not unmount() -- unmount() ejects on Windows");
        return false;
    }
    bool release(const std::string &devicePath, std::string &errorMessage) override
    {
        if (releaseShouldFail) {
            errorMessage = "fake release failure";
            return false;
        }
        releasedPaths.push_back(devicePath);
        return true;
    }
};

class FakeFormatter : public UsbFormatter
{
public:
    std::optional<std::uint64_t> fat32Limit;
    bool formatCalled = false;
    std::string lastWholeDiskPath;
    UsbFilesystem lastFs = UsbFilesystem::Fat32;
    std::string lastLabel;
    bool formatShouldSucceed = true;

    std::optional<std::uint64_t> maxSizeFor(UsbFilesystem fs) const override
    {
        return fs == UsbFilesystem::Fat32 ? fat32Limit : std::nullopt;
    }
    bool format(const std::string &wholeDiskPath, UsbFilesystem fs, const std::string &volumeLabel,
                std::string &errorMessage, ProgressReporter &progress) override
    {
        formatCalled = true;
        lastWholeDiskPath = wholeDiskPath;
        lastFs = fs;
        lastLabel = volumeLabel;
        progress.start("test", 0);
        progress.finish();
        if (!formatShouldSucceed) {
            errorMessage = "fake format failure";
            return false;
        }
        return true;
    }
};

// Unlabelled and unpartitioned, the drive this feature exists for. The
// identity carries the capacity because every real locator copies it in
// (see DetectedStick::identity), and the staleness check compares it.
DetectedStick makeDisk(std::string wholeDiskPath, std::uint64_t capacityBytes)
{
    DetectedStick d;
    d.wholeDiskPath = std::move(wholeDiskPath);
    d.capacityBytes = capacityBytes;
    d.identity.capacityBytes = capacityBytes;
    return d;
}

// The same, with something to be recognised by: a stick that has been
// used before.
DetectedStick makeKnownDisk(std::string wholeDiskPath, std::uint64_t capacityBytes, std::string label,
                            std::string filesystemUuid)
{
    DetectedStick d = makeDisk(std::move(wholeDiskPath), capacityBytes);
    d.label = label;
    d.identity.label = std::move(label);
    d.identity.filesystemUuid = std::move(filesystemUuid);
    return d;
}

}  // namespace

int main()
{
    // A path FormatUsbStick doesn't currently see from the locator is
    // refused outright -- never trust a caller-supplied path, even if it
    // looks plausible.
    {
        FakeLocator locator;
        locator.disks = {makeDisk("/dev/sdb", 32ULL * 1024 * 1024 * 1024)};
        FakeMounter mounter;
        FakeFormatter formatter;
        FormatUsbStick useCase(locator, mounter, formatter);

        std::string error;
        bool ok =
            useCase.execute("/dev/sdc", locator.disks.front().identity, UsbFilesystem::Fat32, "LABEL", error, NullProgressReporter::instance());
        assert(!ok);
        assert(!error.empty());
        assert(!formatter.formatCalled);
        std::cout << "case 1 (unknown path refused) OK\n";
    }

    // A filesystem/size combination the platform can't actually create is
    // refused before format() is ever called -- this is the "wipe then
    // discover it can't format" trap the plan documents (Windows'
    // Format-Volume accepts an oversized FAT32 request at the parameter
    // level and only fails once the disk is already gone).
    {
        FakeLocator locator;
        locator.disks = {makeDisk("/dev/sdb", 64ULL * 1024 * 1024 * 1024)};
        FakeMounter mounter;
        FakeFormatter formatter;
        formatter.fat32Limit = 32ULL * 1024 * 1024 * 1024;
        FormatUsbStick useCase(locator, mounter, formatter);

        std::string error;
        bool ok =
            useCase.execute("/dev/sdb", locator.disks.front().identity, UsbFilesystem::Fat32, "LABEL", error, NullProgressReporter::instance());
        assert(!ok);
        assert(!error.empty());
        assert(!formatter.formatCalled);
        std::cout << "case 2 (oversized FAT32 refused before any destructive call) OK\n";
    }

    // Every mounted partition on the target disk is unmounted before
    // formatting.
    {
        FakeLocator locator;
        auto disk = makeDisk("/dev/sdb", 8ULL * 1024 * 1024 * 1024);
        auto partition = disk;
        partition.devicePath = "/dev/sdb1";
        partition.mounted = true;
        locator.disks = {disk, partition};
        FakeMounter mounter;
        FakeFormatter formatter;
        FormatUsbStick useCase(locator, mounter, formatter);

        std::string error;
        bool ok =
            useCase.execute("/dev/sdb", locator.disks.front().identity, UsbFilesystem::ExFat, "LABEL", error, NullProgressReporter::instance());
        assert(ok);
        assert(mounter.releasedPaths.size() == 1);
        assert(mounter.releasedPaths[0] == "/dev/sdb1");
        assert(formatter.formatCalled);
        std::cout << "case 3 (releases before formatting) OK\n";
    }

    // A failed release stops the whole operation before formatting is
    // ever attempted.
    {
        FakeLocator locator;
        auto disk = makeDisk("/dev/sdb", 8ULL * 1024 * 1024 * 1024);
        auto partition = disk;
        partition.devicePath = "/dev/sdb1";
        partition.mounted = true;
        locator.disks = {disk, partition};
        FakeMounter mounter;
        mounter.releaseShouldFail = true;
        FakeFormatter formatter;
        FormatUsbStick useCase(locator, mounter, formatter);

        std::string error;
        bool ok =
            useCase.execute("/dev/sdb", locator.disks.front().identity, UsbFilesystem::Fat32, "LABEL", error, NullProgressReporter::instance());
        assert(!ok);
        assert(!formatter.formatCalled);
        std::cout << "case 4 (release failure refuses formatting) OK\n";
    }

    // A successful run passes the exact args through to the formatter.
    {
        FakeLocator locator;
        locator.disks = {makeDisk("/dev/sdb", 8ULL * 1024 * 1024 * 1024)};
        FakeMounter mounter;
        FakeFormatter formatter;
        FormatUsbStick useCase(locator, mounter, formatter);

        std::string error;
        bool ok =
            useCase.execute("/dev/sdb", locator.disks.front().identity, UsbFilesystem::ExFat, "MYLABEL", error, NullProgressReporter::instance());
        assert(ok);
        assert(formatter.lastWholeDiskPath == "/dev/sdb");
        assert(formatter.lastFs == UsbFilesystem::ExFat);
        assert(formatter.lastLabel == "MYLABEL");
        std::cout << "case 5 (successful format passes exact args) OK\n";
    }

    // What the app owes a person who just formatted a stick: a stick.
    // Until this landed, execute() returned as soon as the formatter did
    // and left the mounting to the desktop's device notifier -- true on a
    // Plasma or Finder session, false headless, where the rig formatted a
    // stick and every check after it looked at a drive that was not
    // there.
    {
        FakeLocator locator;
        auto disk = makeDisk("/dev/sdb", 8ULL * 1024 * 1024 * 1024);
        disk.devicePath = "/dev/sdb1";
        disk.mounted = false;
        locator.disks = {disk};
        FakeMounter mounter;
        FakeFormatter formatter;
        FormatUsbStick useCase(locator, mounter, formatter);

        std::string error;
        assert(useCase.execute("/dev/sdb", locator.disks.front().identity, UsbFilesystem::Fat32, "LABEL", error, NullProgressReporter::instance()));
        assert(mounter.mountedPaths.size() == 1);
        assert(mounter.mountedPaths.front() == "/dev/sdb1");
        std::cout << "case 6 (a successful format mounts what it made) OK\n";
    }

    // And a mount that cannot be had does not turn a format that worked
    // into a failure: the partition table is written either way, and the
    // desktop may still mount it a moment later.
    {
        FakeLocator locator;
        auto disk = makeDisk("/dev/sdb", 8ULL * 1024 * 1024 * 1024);
        disk.devicePath = "/dev/sdb1";
        disk.mounted = false;
        locator.disks = {disk};
        FakeMounter mounter;
        mounter.mountShouldFail = true;
        FakeFormatter formatter;
        FormatUsbStick useCase(locator, mounter, formatter);

        std::string error;
        assert(useCase.execute("/dev/sdb", locator.disks.front().identity, UsbFilesystem::Fat32, "LABEL", error, NullProgressReporter::instance()));
        assert(formatter.formatCalled);
        assert(error.empty());
        std::cout << "case 7 (a format that worked survives a mount that did not) OK\n";
    }

    // A device node belongs to whatever is plugged into that port. The
    // list a person picked from is a moment old, and the check that the
    // path is still there says nothing about whose path it is now.
    {
        FakeLocator locator;
        locator.disks = {makeKnownDisk("/dev/sdb", 32ULL * 1024 * 1024 * 1024, "MY-SET", "1234ABCD")};
        const StickIdentity chosen = locator.disks.front().identity;
        // Unplugged, and someone else's stick of the same size is in that
        // port by the time Format is pressed.
        locator.disks = {makeKnownDisk("/dev/sdb", 32ULL * 1024 * 1024 * 1024, "WEDDING-2026", "DEADBEEF")};
        FakeMounter mounter;
        FakeFormatter formatter;
        FormatUsbStick useCase(locator, mounter, formatter);

        std::string error;
        const bool ok =
            useCase.execute("/dev/sdb", chosen, UsbFilesystem::Fat32, "LABEL", error, NullProgressReporter::instance());
        assert(!ok);
        assert(!error.empty());
        assert(!formatter.formatCalled && "and nothing was wiped finding out");
        assert(mounter.releasedPaths.empty() && "not even unmounted: the refusal comes before anything is touched");
        std::cout << "case 8 (a different stick in the same port is refused) OK\n";
    }

    // The other direction, and the one a person actually hits: the drive
    // chosen was blank, and by now it is a stick with a library on it.
    {
        FakeLocator locator;
        locator.disks = {makeDisk("/dev/sdb", 32ULL * 1024 * 1024 * 1024)};
        const StickIdentity chosen = locator.disks.front().identity;
        locator.disks = {makeKnownDisk("/dev/sdb", 32ULL * 1024 * 1024 * 1024, "MY-SET", "1234ABCD")};
        FakeMounter mounter;
        FakeFormatter formatter;
        FormatUsbStick useCase(locator, mounter, formatter);

        std::string error;
        assert(!useCase.execute("/dev/sdb", chosen, UsbFilesystem::Fat32, "LABEL", error,
                                NullProgressReporter::instance()));
        assert(!formatter.formatCalled);
        std::cout << "case 9 (a blank drive replaced by a labelled stick is refused) OK\n";
    }

    // And the case the whole feature is for still goes through: a blank,
    // unlabelled drive has no identity to match, and refusing it on that
    // basis would refuse every new stick.
    {
        FakeLocator locator;
        locator.disks = {makeDisk("/dev/sdb", 32ULL * 1024 * 1024 * 1024)};
        FakeMounter mounter;
        FakeFormatter formatter;
        FormatUsbStick useCase(locator, mounter, formatter);

        std::string error;
        assert(useCase.execute("/dev/sdb", locator.disks.front().identity, UsbFilesystem::ExFat, "LABEL", error,
                               NullProgressReporter::instance()));
        assert(formatter.formatCalled);
        std::cout << "case 10 (a blank unlabelled drive is still formattable) OK\n";
    }

    // The direction every real drive takes: a locator always gives a
    // drive a label (model name, drive letter, bsd name at worst), so
    // production goes through isSameStick(), and it has to let the drive
    // that IS the same drive through.
    {
        FakeLocator locator;
        locator.disks = {makeKnownDisk("/dev/sdb", 32ULL * 1024 * 1024 * 1024, "MY-SET", "1234ABCD")};
        FakeMounter mounter;
        FakeFormatter formatter;
        FormatUsbStick useCase(locator, mounter, formatter);

        std::string error;
        assert(useCase.execute("/dev/sdb", locator.disks.front().identity, UsbFilesystem::ExFat, "LABEL", error,
                               NullProgressReporter::instance()));
        assert(formatter.formatCalled);
        std::cout << "case 11 (the stick that is still the stick is formatted) OK\n";
    }

    // One disk, two partitions: every locator reports it twice under one
    // wholeDiskPath, with a filesystem UUID per partition. Comparing
    // against the first entry only refuses such a stick for good, which
    // is worse than the hole the check closes -- a Windows installer
    // stick is exactly the drive someone wants to reformat.
    {
        FakeLocator locator;
        auto esp = makeKnownDisk("/dev/sdb", 32ULL * 1024 * 1024 * 1024, "BOOT", "AAAA1111");
        esp.devicePath = "/dev/sdb1";
        auto data = makeKnownDisk("/dev/sdb", 32ULL * 1024 * 1024 * 1024, "INSTALLER", "BBBB2222");
        data.devicePath = "/dev/sdb2";
        locator.disks = {esp, data};
        FakeMounter mounter;
        FakeFormatter formatter;
        FormatUsbStick useCase(locator, mounter, formatter);

        // The caller listed the second partition, the loop finds the
        // first: both are the same drive and the format goes ahead.
        std::string error;
        const bool ok = useCase.execute("/dev/sdb", data.identity, UsbFilesystem::ExFat, "LABEL", error,
                                        NullProgressReporter::instance());
        if (!ok) {
            std::cerr << "two partitions on one disk were refused: " << error << "\n";
        }
        assert(ok);
        assert(formatter.formatCalled);
        std::cout << "case 12 (a two-partition stick is still one drive) OK\n";
    }

    // The limit of the whole idea, written down as a case rather than
    // left for someone to discover: two blank sticks of one size, in one
    // port, with nothing but a port-derived label (what a locator falls
    // back to when a drive offers no serial, no UUID and no model) are
    // the same drive as far as anything here can tell, and the format
    // goes ahead. Refusing that would refuse every new stick.
    {
        FakeLocator locator;
        locator.disks = {makeKnownDisk("/dev/sdb", 32ULL * 1024 * 1024 * 1024, "sdb", "")};
        const StickIdentity chosen = locator.disks.front().identity;
        // A different stick, indistinguishable: same size, same port, and
        // the same fallback label derived from that port.
        locator.disks = {makeKnownDisk("/dev/sdb", 32ULL * 1024 * 1024 * 1024, "sdb", "")};
        FakeMounter mounter;
        FakeFormatter formatter;
        FormatUsbStick useCase(locator, mounter, formatter);

        std::string error;
        assert(useCase.execute("/dev/sdb", chosen, UsbFilesystem::ExFat, "LABEL", error,
                               NullProgressReporter::instance())
               && "with no evidence of a difference there is nothing to refuse");
        assert(formatter.formatCalled);
        std::cout << "case 13 (two indistinguishable blank sticks: the check cannot help, and says so) OK\n";
    }

    std::cout << "All format_usb_stick tests passed.\n";
    return 0;
}
