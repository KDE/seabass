// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "application/ports/cancellation_token.hpp"

namespace seabass::application
{

struct CompactionPreflight
{
    std::string error;  // non-empty: archive unreadable
    std::uint64_t archiveBytes = 0;
    std::uint64_t liveBytes = 0;
    std::uint64_t deadBytes = 0;
    double deadRatio = 0.0;
    bool suggested = false;  // past the design's threshold
    std::uint64_t requiredFreeBytes = 0;  // for the temporary copy
    std::uint64_t availableFreeBytes = 0;
    bool enoughFreeSpace = false;
};

struct CompactStickBackupOptions
{
    std::filesystem::path archivePath;
    CancellationToken cancel = CancellationToken::none();
    std::function<void(std::uint64_t bytesDone, std::uint64_t bytesTotal)> onProgress;
    std::uint64_t freeSpaceMarginBytes = 64u << 20;
};

struct CompactionOutcome
{
    enum class Status
    {
        Compacted,
        NothingToReclaim,
        NotEnoughSpace,
        Cancelled,
        Failed,
    };
    Status status = Status::Failed;
    std::string message;
    std::uint64_t bytesBefore = 0;
    std::uint64_t bytesAfter = 0;
    std::uint64_t requiredFreeBytes = 0;
    std::uint64_t availableFreeBytes = 0;
};

// Copy-to-temp + verify + atomic rename (docs/stick-backup-plan.md, "Dead
// space and compaction"). The old archive stays valid and browsable
// until the rename; a crash leaves it intact plus a `.compacting` temp
// file that the next run removes. Needs free space for the live bytes --
// refused with exact numbers otherwise. Never touches the stick.
class CompactStickBackup
{
public:
    static CompactionPreflight preflight(const std::filesystem::path &archivePath, std::uint64_t freeSpaceMarginBytes = 64u << 20);
    static CompactionOutcome execute(const CompactStickBackupOptions &options);
    static std::filesystem::path temporaryPathFor(const std::filesystem::path &archivePath);
};

}  // namespace seabass::application
