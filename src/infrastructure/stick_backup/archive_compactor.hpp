// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <functional>

#include "application/ports/cancellation_token.hpp"
#include "infrastructure/stick_backup/archive_file.hpp"
#include "infrastructure/stick_backup/archive_stats.hpp"
#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"

namespace seabass::infrastructure::stick_backup
{

// When to *suggest* compaction (never run it unasked): a fifth of the
// file is dead, or 5 GB is, whichever comes first -- on a very large
// archive the ratio alone would let tens of GB sit dead.
constexpr double SuggestCompactionRatio = 0.20;
constexpr std::uint64_t SuggestCompactionBytes = 5'000'000'000ull;

bool shouldSuggestCompaction(const DeadSpaceReport &report);

struct CompactionResult
{
    std::uint64_t bytesBefore = 0;
    std::uint64_t bytesAfter = 0;
    std::size_t entries = 0;  // excluding the manifest
    bool cancelled = false;
};

// Streams every live entry of `source` into `destination` (a fresh,
// empty ArchiveFile), verifying each one against the manifest on the way
// -- CRC32 from the central directory and SHA-256 from the manifest row
// -- so compaction doubles as a full verify. The manifest is written
// unchanged: compaction alters nothing about *what* is backed up, only
// where the bytes sit. Throws ArchiveFormatError on the first entry that
// fails verification; the destination is then garbage and the caller
// discards it. Never touches the stick. `progress` receives bytes copied
// so far.
CompactionResult compactArchive(const Zip64Reader &source, const BackupManifest &manifest, ArchiveFile &destination,
                                application::CancellationToken cancel = application::CancellationToken::none(),
                                const std::function<void(std::uint64_t, std::uint64_t)> &progress = {});

}  // namespace seabass::infrastructure::stick_backup
