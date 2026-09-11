// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "application/ports/progress_reporter.hpp"
#include "domain/usb_filesystem.hpp"

namespace seabass::application
{

// Port for formatting a whole removable disk with a fresh MBR partition
// table and a single primary partition -- the only partitioning scheme
// this project ever creates, since GPT is a documented "formats fine,
// player refuses it" failure mode on real CDJ/XDJ/Engine OS hardware.
class UsbFormatter
{
public:
    virtual ~UsbFormatter() = default;

    // The largest volume this platform can actually create for the given
    // filesystem, if there's a real ceiling -- nullopt means no known
    // limit. Exists because Windows' own filesystem-creation path refuses
    // to create a FAT32 volume over 32GB (a Microsoft-imposed limit, not
    // a filesystem one); Linux's mkfs.vfat has no such ceiling. Callers
    // MUST check this before calling format() -- see
    // application::use_cases::FormatUsbStick, which refuses the whole
    // operation up front rather than discovering the failure after
    // the disk has already been wiped.
    virtual std::optional<std::uint64_t> maxSizeFor(domain::UsbFilesystem fs) const = 0;

    // wholeDiskPath is the *disk*, not a partition (e.g. "/dev/sdb", not
    // "/dev/sdb1") -- this destroys any existing partition table and
    // everything on the disk. On success, the disk has one MBR primary
    // partition spanning the whole disk, formatted as `fs` with
    // `volumeLabel`. On failure, returns false and fills errorMessage.
    virtual bool format(const std::string &wholeDiskPath, domain::UsbFilesystem fs, const std::string &volumeLabel,
                         std::string &errorMessage, ProgressReporter &progress) = 0;
};

}  // namespace seabass::application
