// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

#include "application/ports/progress_reporter.hpp"
#include "application/ports/removable_media_locator.hpp"
#include "application/ports/removable_media_mounter.hpp"
#include "application/ports/usb_formatter.hpp"
#include "domain/usb_filesystem.hpp"

namespace seabass::application
{

// Orchestration only, no formatting logic of its own -- see UsbFormatter
// for the actual per-platform work. Exists to enforce two safety
// invariants no caller should be able to bypass, both learned from real
// findings while designing this feature (see the approved plan):
//
// 1. Never format a path a caller merely typed or remembered -- re-runs
//    RemovableMediaLocator::detect() and only proceeds if wholeDiskPath
//    is still present in the fresh result, the same "never trust a
//    stale/merely-typed path" discipline PdbRowWriter already established
//    for file writes.
// 2. Never start a destructive operation that's already known to fail --
//    checks UsbFormatter::maxSizeFor() *before* unmounting or formatting
//    anything, because on Windows Format-Volume accepts a FAT32 request
//    on an oversized drive at the parameter level and only fails once the
//    disk has already been wiped by the preceding Clear-Disk. Refusing up
//    front means that combination never reaches a destructive call at all.
class FormatUsbStick
{
public:
    FormatUsbStick(RemovableMediaLocator &locator, RemovableMediaMounter &mounter, UsbFormatter &formatter)
        : m_locator(locator), m_mounter(mounter), m_formatter(formatter)
    {
    }

    bool execute(const std::string &wholeDiskPath, domain::UsbFilesystem fs, const std::string &volumeLabel,
                 std::string &errorMessage, ProgressReporter &progress)
    {
        auto disks = m_locator.detect();

        const DetectedStick *target = nullptr;
        for (const auto &disk : disks) {
            if (disk.wholeDiskPath == wholeDiskPath) {
                target = &disk;
                break;
            }
        }
        if (target == nullptr) {
            errorMessage = "That drive is no longer present. Reconnect it and try again.";
            return false;
        }

        auto maxSize = m_formatter.maxSizeFor(fs);
        if (maxSize && target->capacityBytes > *maxSize) {
            errorMessage = domain::usbFilesystemName(fs) + " can't be created on a drive this large on this "
                           "platform. Choose a different format.";
            return false;
        }

        // Release every currently-mounted partition on this disk before
        // touching anything -- a formatter that tries to repartition a
        // disk with a mounted partition on it will, at best, fail
        // cleanly and, at worst, behave unpredictably. Deliberately
        // release(), not unmount(): this use case keeps operating on the
        // same disk immediately afterwards, and unmount() on Windows
        // physically ejects the media, which leaves it without a usable
        // volume until reinserted -- see the port's comment.
        for (const auto &disk : disks) {
            if (disk.wholeDiskPath != wholeDiskPath || !disk.mounted || disk.devicePath.empty()) {
                continue;
            }
            std::string releaseError;
            if (!m_mounter.release(disk.devicePath, releaseError)) {
                errorMessage = "Couldn't unmount " + disk.devicePath + " first: " + releaseError;
                return false;
            }
        }

        return m_formatter.format(wholeDiskPath, fs, volumeLabel, errorMessage, progress);
    }

private:
    RemovableMediaLocator &m_locator;
    RemovableMediaMounter &m_mounter;
    UsbFormatter &m_formatter;
};

}  // namespace seabass::application
