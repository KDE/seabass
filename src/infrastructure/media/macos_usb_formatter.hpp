// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include "application/ports/usb_formatter.hpp"

namespace seabass::infrastructure::media
{

// Formats a whole USB disk with "diskutil eraseDisk ... MBRFormat", which
// writes a fresh MBR partition table with one partition and formats it in
// a single step.
class MacUsbFormatter : public application::UsbFormatter
{
public:
    std::optional<std::uint64_t> maxSizeFor(domain::UsbFilesystem fs) const override;

    bool format(const std::string &wholeDiskPath, domain::UsbFilesystem fs, const std::string &volumeLabel,
                std::string &errorMessage, application::ProgressReporter &progress) override;
};

}  // namespace seabass::infrastructure::media
