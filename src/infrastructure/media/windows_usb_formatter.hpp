// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "application/ports/usb_formatter.hpp"

namespace seabass::infrastructure::media
{

// Formats a whole USB disk via PowerShell's Storage module (Clear-Disk /
// Initialize-Disk / New-Partition / Format-Volume) -- NOT diskpart.
// diskpart's manifest is requireAdministrator, so even a read-only
// diskpart call fails at process-creation (ERROR_ELEVATION_REQUIRED)
// without elevation, with nothing useful on stdout/stderr to parse;
// verified on real hardware (see the approved plan). The Storage module
// gives real structured errors and, crucially, is the same module already
// confirmed to enumerate disks *without* elevation, so
// WindowsRemovableMediaLocator and this formatter share one consistent
// API instead of two different tools.
//
// Elevation only happens here, at the moment of format -- enumeration
// (WindowsRemovableMediaLocator) never needs it. Because a process
// launched via ShellExecuteExW's "runas" verb has no stdout/stderr pipe
// back to the parent (verified: this isn't optional plumbing that could
// be added, it's how UAC-elevated process creation works), the mutating
// script writes its own outcome to a result file instead of being
// captured from output.
class WindowsUsbFormatter : public application::UsbFormatter
{
public:
    std::optional<std::uint64_t> maxSizeFor(domain::UsbFilesystem fs) const override;

    bool format(const std::string &wholeDiskPath, domain::UsbFilesystem fs, const std::string &volumeLabel,
                std::string &errorMessage, application::ProgressReporter &progress) override;
};

}  // namespace seabass::infrastructure::media
