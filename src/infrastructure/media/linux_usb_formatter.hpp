// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "application/ports/usb_formatter.hpp"

namespace seabass::infrastructure::media
{

// Formats a whole USB disk via udisks2's own D-Bus API (the same daemon
// this project already drives for mount/unmount, see
// UdisksctlMediaMounter) rather than shelling out to parted/mkfs directly
// or hand-rolling partition tables. Talks to udisks2 through "gdbus call"
// (present on every system with GLib, no new dependency) rather than
// linking libudisks2/QtDBus for two calls.
// The MBR partition-type byte a formatted stick gets stamped with, as the
// hex string udisks2 wants. Exposed (rather than kept anonymous-namespace
// private) so it has direct unit test coverage: the value used to be left
// for udisks2 to choose, it chose 0x83 (Linux) for a FAT32 stick, and macOS
// then offered to erase the result -- a silent wrong answer that only a real
// stick on a second operating system revealed.
std::string mbrPartitionType(domain::UsbFilesystem fs);

class LinuxUsbFormatter : public application::UsbFormatter
{
public:
    std::optional<std::uint64_t> maxSizeFor(domain::UsbFilesystem fs) const override;

    bool format(const std::string &wholeDiskPath, domain::UsbFilesystem fs, const std::string &volumeLabel,
                std::string &errorMessage, application::ProgressReporter &progress) override;
};

}  // namespace seabass::infrastructure::media
