// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>

#include "infrastructure/stick_backup/zip64_reader.hpp"

namespace seabass::infrastructure::stick_backup
{

struct DeadSpaceReport
{
    std::uint64_t fileSize = 0;
    std::uint64_t liveBytes = 0;      // every listed entry's header + data + descriptor
    std::uint64_t overheadBytes = 0;  // the current central directory and trailer
    std::uint64_t deadBytes = 0;      // everything else: old trailers, replaced and removed entries
    double deadRatio() const { return fileSize == 0 ? 0.0 : static_cast<double>(deadBytes) / static_cast<double>(fileSize); }
};

// Exact, because we are the only writer: dead = file size - live - overhead.
// Reads one local header per entry (to know each header's length), no
// entry data.
inline DeadSpaceReport deadSpace(const Zip64Reader &reader)
{
    DeadSpaceReport report;
    report.fileSize = reader.layout().fileSize;
    for (std::size_t i = 0; i < reader.entries().size(); ++i) {
        report.liveBytes += reader.footprint(i);
    }
    report.overheadBytes = report.fileSize - reader.layout().centralDirectoryOffset;
    report.deadBytes = report.fileSize - report.liveBytes - report.overheadBytes;
    return report;
}

}  // namespace seabass::infrastructure::stick_backup
