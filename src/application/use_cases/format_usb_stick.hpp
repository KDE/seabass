// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

#include "application/ports/progress_reporter.hpp"
#include "application/ports/removable_media_locator.hpp"
#include <chrono>
#include <thread>
#include "application/ports/removable_media_mounter.hpp"
#include "application/ports/usb_formatter.hpp"
#include "application/stick_identity.hpp"
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
//    for file writes. Present is not enough, though: a device node is
//    handed to whatever is plugged in next (StickIdentity exists because
//    devicePath and mountPoint are reassigned on every replug), so the
//    drive found there must also be the drive the caller chose. Without
//    that, unplugging the stick shown in the list and plugging in
//    another one before pressing Format wipes the second.
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

    // chosen: who the drive was when the caller decided to erase it (the
    // identity out of the same detect() the caller listed). Compared
    // against who is at that path now; see sameDrive() below for the
    // blank-drive case, which has no identity to compare and is the
    // commonest reason to format anything.
    bool execute(const std::string &wholeDiskPath, const StickIdentity &chosen, domain::UsbFilesystem fs,
                 const std::string &volumeLabel, std::string &errorMessage, ProgressReporter &progress)
    {
        auto disks = m_locator.detect();

        // One disk, one entry per PARTITION: every locator here reports a
        // stick with two filesystems twice, both carrying the same
        // wholeDiskPath and a filesystem UUID of their own. So the
        // identity is checked against every entry for this disk rather
        // than the first one found -- matching only the first refuses a
        // two-partition stick (a Windows installer stick, say) for good,
        // whichever of its partitions the caller happened to list.
        const DetectedStick *target = nullptr;
        bool isTheDriveThatWasChosen = false;
        for (const auto &disk : disks) {
            if (disk.wholeDiskPath != wholeDiskPath) {
                continue;
            }
            if (target == nullptr) {
                target = &disk;
            }
            if (sameDrive(chosen, disk.identity, disk.capacityBytes)) {
                isTheDriveThatWasChosen = true;
            }
        }
        if (target == nullptr) {
            errorMessage = "That drive is no longer present. Reconnect it and try again.";
            return false;
        }

        if (!isTheDriveThatWasChosen) {
            errorMessage = "The drive at that connection is not the one you chose. Refresh the list and pick it "
                           "again.";
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

        if (!m_formatter.format(wholeDiskPath, fs, volumeLabel, errorMessage, progress)) {
            return false;
        }

        // Mount what we just made. Until now this returned as soon as the
        // formatter did and left the mounting to the desktop, which is
        // true of a Plasma or Finder session with a device notifier
        // running and false everywhere else: the rig formats a stick
        // headlessly and the drive simply never came back, so every check
        // after it was looking at a stick that was not there. A person
        // who formats a stick inside this app is asking for a stick to
        // use, not for a partition table.
        //
        // Best effort by design. A format that worked is not undone by a
        // mount that did not, and the desktop may well mount it a moment
        // later anyway -- so a failure here is reported through the
        // return value only if the caller asked for the mount point,
        // which nothing does yet, and never turns a successful format
        // into a failed one.
        // Six attempts over three seconds: long enough for udisks to
        // publish a freshly written partition, short enough that a
        // machine where mounting simply is not going to work costs a
        // test three seconds rather than a minute.
        for (int attempt = 0; attempt < 6; ++attempt) {
            for (const auto &disk : m_locator.detect()) {
                if (disk.wholeDiskPath != wholeDiskPath || disk.devicePath.empty()) {
                    continue;
                }
                if (disk.mounted && !disk.mountPoint.empty()) {
                    return true;
                }
                std::string mountError;
                if (m_mounter.mount(disk.devicePath, mountError)) {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return true;
    }

    // "Still the same drive", for a decision that destroys everything on
    // it. StickIdentity::isSameStick() is the rule wherever there is
    // anything to key on; what it cannot answer is the blank unlabelled
    // drive, where it says no to two readings of one drive because
    // neither side has a label, a UUID or a serial. That case is the
    // reason this feature exists, so it is decided here instead: nothing
    // to key on on EITHER side, and the same size, passes. A drive that
    // had nothing and now has a label (someone else's stick in the same
    // port) does not, and neither does the reverse.
    // What this cannot do, said out loud: where the only evidence is a
    // label and a capacity, and the label is one of the port-derived
    // fallbacks a locator uses when a drive offers nothing better (the
    // devnode name on Linux, the PhysicalDrive path on Windows), two
    // same-size blank sticks in the same port compare equal, because
    // nothing observable tells them apart. The check refuses what it can
    // see to be a different drive; it cannot invent evidence, and
    // refusing whenever evidence is thin would refuse every cheap new
    // stick, which is the drive this feature exists to format.
    //
    // Public because the page that offers the drive asks the same
    // question when its list changes under the selection: one rule, one
    // definition, rather than a second opinion in the GUI.
    static bool sameDrive(const StickIdentity &chosen, const StickIdentity &found, std::uint64_t foundCapacityBytes)
    {
        const bool chosenIsAnonymous = chosen.strength() == StickIdentity::Strength::None;
        const bool foundIsAnonymous = found.strength() == StickIdentity::Strength::None;
        if (chosenIsAnonymous || foundIsAnonymous) {
            if (chosenIsAnonymous != foundIsAnonymous) {
                return false;
            }
            // capacityBytes is copied into the identity by every locator,
            // but a caller that built one by hand may not have; the
            // detected capacity is the one to trust for the found side.
            return chosen.capacityBytes == foundCapacityBytes;
        }
        return chosen.isSameStick(found);
    }

private:
    RemovableMediaLocator &m_locator;
    RemovableMediaMounter &m_mounter;
    UsbFormatter &m_formatter;
};

}  // namespace seabass::application
