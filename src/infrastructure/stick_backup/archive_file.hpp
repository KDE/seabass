// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace seabass::infrastructure::stick_backup
{

class ArchiveIoError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// The byte-level file the stick backup archive (and its journal) is
// written through. Deliberately tiny and append-only -- there is no
// writeAt(), because the whole crash-safety argument in
// docs/stick-backup-plan.md rests on never modifying bytes that a
// previously written end-of-central-directory record already covers.
//
// Two implementations: PosixArchiveFile (a real file; POSIX fd or Win32
// HANDLE) and InMemoryArchiveFile (tests). Everything in this directory
// -- writer, reader, updater, compactor -- takes an ArchiveFile&, never a
// path, so the exhaustive truncation and write-reordering tests run
// entirely in memory against the same code that touches disk.
//
// All operations throw ArchiveIoError on failure; a short read is a
// failure, not a partial result.
class ArchiveFile
{
public:
    virtual ~ArchiveFile() = default;

    virtual std::uint64_t size() const = 0;

    virtual void append(std::span<const std::byte> bytes) = 0;

    // Fills `out` completely from `offset`; throws if the range runs past
    // the end of the file.
    virtual void readAt(std::uint64_t offset, std::span<std::byte> out) const = 0;

    virtual void truncate(std::uint64_t newSize) = 0;

    // Durability barrier: everything appended or truncated before this
    // call is on the medium when it returns (fsync; F_FULLFSYNC on macOS
    // because plain fsync there does not flush the drive cache;
    // FlushFileBuffers on Windows). The placement of these calls is what
    // the write-reordering fault-injection tests check.
    virtual void barrier() = 0;
};

}  // namespace seabass::infrastructure::stick_backup
