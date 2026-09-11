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
class LinuxUsbFormatter : public application::UsbFormatter
{
public:
    std::optional<std::uint64_t> maxSizeFor(domain::UsbFilesystem fs) const override;

    bool format(const std::string &wholeDiskPath, domain::UsbFilesystem fs, const std::string &volumeLabel,
                std::string &errorMessage, application::ProgressReporter &progress) override;
};

}  // namespace seabass::infrastructure::media
