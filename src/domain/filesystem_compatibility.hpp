// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

namespace seabass::domain
{

// Static knowledge about a filesystem's suitability for a DJ USB stick --
// no filesystem/OS access here, callers (infrastructure) detect which
// filesystem is actually in use and look it up here. Deliberately
// conservative: real DJ hardware compatibility varies by exact model and
// firmware version in ways this project has no way to verify against a
// hardware matrix, so notes are phrased as general guidance ("older
// units", "check your player's manual") rather than claiming specific
// model/firmware cutoffs this project hasn't actually tested.
struct FilesystemCompatibilityInfo
{
    std::string displayName;    // e.g. "FAT32", "exFAT", "NTFS"
    std::string maxFileSize;    // human-readable, e.g. "4 GiB minus 1 byte"
    std::string hardwareNotes;  // free-text compatibility summary
    // Whether this project considers the filesystem a reasonable choice
    // for a stick meant to play on real CDJ/XDJ/Engine OS hardware, as
    // opposed to "technically what Seabass can read, but you probably
    // don't want this on a stick you're taking to a gig."
    bool recommendedForDjHardware = false;
};

class FilesystemCompatibility
{
public:
    // filesystem is the lowercase type name as reported by the OS (e.g.
    // "vfat", "exfat", "ntfs", "ext4", "hfsplus", "apfs"). Unknown values
    // get a generic "unrecognized" entry rather than a made-up one.
    static FilesystemCompatibilityInfo lookup(const std::string &filesystem);
};

}  // namespace seabass::domain
