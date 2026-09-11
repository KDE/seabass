// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <string>

namespace seabass::domain
{

// The filesystem to format a USB stick as. Deliberately just these two --
// every CDJ/XDJ/Engine OS player either reads FAT32 or exFAT, nothing
// else, and this project has no macOS host build to make HFS+ meaningful
// to offer, so GPT/HFS+/NTFS are all out of scope.
enum class UsbFilesystem
{
    Fat32,
    ExFat,
};

// Display name only -- e.g. for log lines and error messages. UI copy
// should use its own plain-language labels ("Works on every player"),
// not this.
std::string usbFilesystemName(UsbFilesystem fs);

// The ≤32GB->FAT32 / >32GB->exFAT default: 4GB-file-cap FAT32 essentially
// never bites at this size, matches where genuinely old hardware still
// lives, and stays clear of Windows' own 32GB-native-FAT32-creation
// ceiling. Pure heuristic, not a hard rule -- both filesystems stay
// manually selectable regardless of what this recommends.
UsbFilesystem recommendedUsbFilesystem(std::uint64_t capacityBytes);

}  // namespace seabass::domain
