// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/media/macos_removable_media_locator.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "infrastructure/media/macos_disk_description.hpp"
#include "infrastructure/media/stick_root_scan.hpp"

namespace seabass::infrastructure::media
{

using application::DetectedStick;

std::vector<DetectedStick> MacRemovableMediaLocator::detect()
{
    const auto all = describeAllDisks();

    std::map<std::string, const MacDiskDescription *> stickDisks;
    for (const auto &disk : all) {
        if (isRemovableStickDisk(disk)) {
            stickDisks[disk.bsdName] = &disk;
        }
    }

    // As on Linux, a disk with partitions is represented by its partitions
    // and not listed itself.
    std::set<std::string> disksWithPartitions;
    std::vector<const MacDiskDescription *> entries;
    for (const auto &disk : all) {
        if (!disk.whole && stickDisks.count(disk.wholeBsdName)) {
            disksWithPartitions.insert(disk.wholeBsdName);
            entries.push_back(&disk);
        }
    }
    for (const auto &[name, disk] : stickDisks) {
        if (!disksWithPartitions.count(name)) {
            entries.push_back(disk);
        }
    }
    // IOKit's order is registration order; sort so the list is stable.
    std::sort(entries.begin(), entries.end(),
              [](const MacDiskDescription *a, const MacDiskDescription *b) { return a->bsdName < b->bsdName; });

    std::vector<DetectedStick> sticks;
    for (const MacDiskDescription *entry : entries) {
        const MacDiskDescription &wholeDisk = *stickDisks.at(entry->wholeBsdName);

        DetectedStick stick;
        stick.devicePath = "/dev/" + entry->bsdName;
        stick.wholeDiskPath = "/dev/" + wholeDisk.bsdName;
        // Format USB Stick works on the whole disk, so the capacity shown
        // is the whole disk's, not this partition's.
        stick.capacityBytes = wholeDisk.sizeBytes;
        if (entry->whole) {
            // Only reached for a disk with no partitions: blank unless a
            // filesystem sits directly on it (superfloppy).
            stick.hasNoFilesystem = entry->content.empty() && entry->volumeKind.empty();
        }

        stick.label = !entry->volumeName.empty() ? entry->volumeName
            : !wholeDisk.model.empty()           ? wholeDisk.model
                                                 : entry->bsdName;

        // Only the built-in reader is recognisable here: an SD card in a
        // USB reader reports plain "USB", the same as a flash drive.
        stick.isSdCard = wholeDisk.protocol == "Secure Digital";

        // Identity (see StickIdentity). The volume UUID is the one macOS
        // reports -- for FAT and exFAT it is derived from the volume
        // serial, so it changes on reformat like ID_FS_UUID does, but it
        // is not spelled the same as on Linux or Windows.
        stick.identity.hardwareSerial = wholeDisk.usbSerial;
        stick.identity.filesystemUuid = entry->volumeUuid;
        stick.identity.label = stick.label;
        stick.identity.capacityBytes = stick.capacityBytes;

        if (!entry->mountPoint.empty()) {
            stick.mounted = true;
            stick.mountPoint = entry->mountPoint;
            scanMountedRoot(entry->mountPoint, stick);
        }

        sticks.push_back(std::move(stick));
    }
    return sticks;
}

}  // namespace seabass::infrastructure::media
