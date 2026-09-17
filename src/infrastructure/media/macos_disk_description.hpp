// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace seabass::infrastructure::media
{

// What DiskArbitration says about one disk or partition, copied out of its
// CFDictionary into plain values so the macOS adapters never pass
// CoreFoundation objects around. Every field is what DADiskCopyDescription
// reported, or its empty value where the key was absent.
struct MacDiskDescription
{
    std::string bsdName;       // "disk4s1"
    std::string wholeBsdName;  // "disk4" -- the same as bsdName for a whole disk
    bool whole = false;
    // Published by an IOBlockStorageDriver, i.e. a real drive rather than
    // a disk image or an APFS container synthesized on top of one.
    bool physical = false;
    bool internal = false;
    bool removable = false;
    std::uint64_t sizeBytes = 0;
    std::string protocol;      // "USB", "Secure Digital", "PCI-Express", "Disk Image", ...
    std::string model;
    std::string content;       // partition scheme or partition type, "" for a blank disk
    std::string volumeKind;    // "msdos", "exfat", ... -- "" when no filesystem was recognised
    std::string volumeName;
    std::string volumeUuid;
    std::string mountPoint;    // "" when not mounted
    std::string usbSerial;     // "" when the device (or its bridge) reports none
};

// A removable drive of the kind Seabass works with: a physical USB drive,
// or an SD card in a built-in or USB reader. Deliberately the same scope as
// LinuxRemovableMediaLocator (ID_BUS == usb) plus built-in SD readers, which
// macOS reports under their own protocol rather than USB.
bool isRemovableStickDisk(const MacDiskDescription &wholeDisk);

// "/dev/disk4s1" or "disk4s1" -> "disk4s1"; nullopt for anything that is
// not a plain /dev/diskN or /dev/diskNsM node, so a caller can never be
// talked into acting on some other path.
std::optional<std::string> bsdNameFromDevicePath(const std::string &devicePath);

// Every disk and partition IOKit currently publishes, described.
std::vector<MacDiskDescription> describeAllDisks();

// Describes one disk by BSD name. nullopt if DiskArbitration does not know
// it (unplugged since, or never existed).
std::optional<MacDiskDescription> describeDisk(const std::string &bsdName);

}  // namespace seabass::infrastructure::media
