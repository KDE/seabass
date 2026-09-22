// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace seabass::cli
{

// Hidden test command: damage a real stick's FAT32 bookkeeping on purpose,
// so the filesystem repair in Library Health can be exercised end to end.
//
// Reaching that state honestly is a matter of luck. Pulling a stick
// mid-write usually leaves a clean filesystem, because udisks mounts
// removable vfat with "flush" and almost nothing is ever in flight; and
// scribbling on the FAT with dd needs root and can cost real data. See
// issue #37.
//
// What it damages, and what it refuses to touch:
//
//   - FAT[1]'s ClnShutBitMask, cleared. That is precisely what a FAT
//     driver does on mount and undoes on a clean unmount, so a cleared
//     bit is the filesystem saying "I was not unmounted cleanly".
//   - FSINFO's free-cluster count and next-free hint, set to nonsense.
//
// Both are the filesystem's own bookkeeping. Directory entries and file
// data are never written, so fsck.fat can put it right and no track is
// ever at risk -- which is the point: the repair is the thing under test,
// and a stick this cannot repair would test nothing.
//
// The guards, in order, and every one of them refuses rather than asks:
//
//   1. Linux only. The recipe is FAT-and-kernel specific.
//   2. The device must be one the app's own RemovableMediaLocator
//      reports. That is a stronger guard than any check written here,
//      because the locator only ever yields USB partitions -- it has no
//      way to name an internal disk, so neither has this.
//   3. It must actually be FAT32, read from its own boot sector.
//   4. The person types the volume label back, as Format USB Stick makes
//      them.
//
// Returns a process exit code. `deviceArg` is a partition ("/dev/sdb1").
int runDamageFilesystemCommand(const std::string &deviceArg, bool confirmedOnCommandLine);

// The byte-level half, exposed so a test can hold it to its promise.
//
// The guards above decide WHETHER to write; these decide WHERE, and a
// wrong answer here is the one that could reach a directory entry or a
// track. So damage_filesystem_test drives them against a real FAT32
// image and compares every byte before and after -- the claim being not
// merely "it set the dirty bit" but "it changed these eight bytes and
// nothing else".

struct Fat32Geometry
{
    std::uint32_t bytesPerSector = 0;
    std::uint32_t reservedSectors = 0;
    std::uint32_t fsInfoSector = 0;
};

// Read out of the volume's own boot sector, never trusted from a mount
// option or a name. nullopt with `why` set for anything that is not
// FAT32, which includes a volume that merely looks the part.
std::optional<Fat32Geometry> readFat32Geometry(const std::string &path, std::string &why);

// Clears FAT[1]'s clean-unmount bit and wrongs FSINFO's two free-cluster
// fields. Eight bytes, at offsets derived from `geometry`. Nothing else
// is written.
bool applyDamage(const std::string &path, const Fat32Geometry &geometry, std::string &why);

}  // namespace seabass::cli
