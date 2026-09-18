// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

namespace seabass::infrastructure::media
{

// Whether the filesystem holding `path` is mounted read-only.
//
// A stick pulled mid-write leaves FAT/exFAT inconsistent, and the kernel
// then refuses to write to it: Linux remounts it read-only on the first
// bad cluster count ("FAT-fs (sdb1): Filesystem has been set read-only"),
// which is how a save of a thousand small files reports a thousand
// failures for one cause. Asked before staging, it is one sentence
// instead ("the stick is read-only"), and asked of a save's own failure
// it names the reason rather than the file that happened to be first.
bool isMountedReadOnly(const std::string &path);

// The device behind a mount point ("/dev/sdb1", "disk4s1", "E:"), or
// empty when it cannot be told. Only used to talk to the platform's own
// repair tool.
std::string deviceForMountPoint(const std::string &mountPoint);

struct FilesystemRepairResult
{
    bool repaired = false;
    // Said to the user as-is: what happened, or why it could not run.
    std::string message;
    // The platform's own permission prompt was declined. Not a failure of
    // the repair, and not something to apologise for -- the user said no.
    bool declined = false;
};

// Runs the platform's own filesystem check-and-repair on the stick at
// `mountPoint`, unmounting and remounting it around the repair where the
// platform needs that.
//
// Nothing here elevates Seabass itself. Each platform asks in its own
// way: polkit on Linux (udisks2 does the work), UAC on Windows (an
// elevated PowerShell, the same consent prompt formatting already uses),
// and diskutil on macOS, which asks only if the volume needs it.
//
// The Windows and macOS paths are written from their vendors' own
// documentation and have never been run on real hardware -- see
// docs/manual-testing.md.
FilesystemRepairResult repairFilesystem(const std::string &mountPoint);

}  // namespace seabass::infrastructure::media
