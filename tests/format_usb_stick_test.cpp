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
    bool releaseShouldFail = false;

    std::optional<std::string> mount(const std::string &, std::string &) override { return std::nullopt; }
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

DetectedStick makeDisk(std::string wholeDiskPath, std::uint64_t capacityBytes)
{
    DetectedStick d;
    d.wholeDiskPath = std::move(wholeDiskPath);
    d.capacityBytes = capacityBytes;
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
            useCase.execute("/dev/sdc", UsbFilesystem::Fat32, "LABEL", error, NullProgressReporter::instance());
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
            useCase.execute("/dev/sdb", UsbFilesystem::Fat32, "LABEL", error, NullProgressReporter::instance());
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
            useCase.execute("/dev/sdb", UsbFilesystem::ExFat, "LABEL", error, NullProgressReporter::instance());
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
            useCase.execute("/dev/sdb", UsbFilesystem::Fat32, "LABEL", error, NullProgressReporter::instance());
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
            useCase.execute("/dev/sdb", UsbFilesystem::ExFat, "MYLABEL", error, NullProgressReporter::instance());
        assert(ok);
        assert(formatter.lastWholeDiskPath == "/dev/sdb");
        assert(formatter.lastFs == UsbFilesystem::ExFat);
        assert(formatter.lastLabel == "MYLABEL");
        std::cout << "case 5 (successful format passes exact args) OK\n";
    }

    std::cout << "All format_usb_stick tests passed.\n";
    return 0;
}
