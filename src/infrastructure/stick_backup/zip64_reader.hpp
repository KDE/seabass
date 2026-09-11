// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "infrastructure/stick_backup/archive_file.hpp"
#include "infrastructure/stick_backup/zip64_writer.hpp"

namespace seabass::infrastructure::stick_backup
{

// Where the trailer structures sit. `centralDirectoryOffset` is also the
// end of the last entry's bytes, so `fileSize - centralDirectoryOffset`
// is the whole overhead term in the dead-space arithmetic.
struct ArchiveLayout
{
    std::uint64_t fileSize = 0;
    std::uint64_t centralDirectoryOffset = 0;
    std::uint64_t centralDirectorySize = 0;
    std::uint64_t zip64EndOfCentralDirectoryOffset = 0;
    std::uint64_t zip64LocatorOffset = 0;
    std::uint64_t endOfCentralDirectoryOffset = 0;
    std::uint64_t entryCount = 0;
};

// Reads archives written by Zip64Writer (and, structurally, any ZIP64
// STORE archive with a single disk). Deliberately strict: every offset
// and count is checked against the file size before it is used, the
// EOCD must sit exactly at the end of the file (we never write a
// comment), and only STORE entries can be read back. A structurally
// broken file throws ArchiveFormatError from open(); it is the caller's
// job (archive_recovery) to decide what that means.
class Zip64Reader
{
public:
    static Zip64Reader open(const ArchiveFile &file);
    // Same, but a format problem yields nullopt (and the message, if
    // asked) instead of throwing. I/O errors still throw.
    static std::optional<Zip64Reader> tryOpen(const ArchiveFile &file, std::string *error = nullptr);

    const std::vector<CentralEntry> &entries() const { return m_entries; }
    const ArchiveLayout &layout() const { return m_layout; }
    std::optional<std::size_t> findEntry(std::string_view name) const;

    // Offset of the entry's first data byte; reads and validates the local
    // header on first use (signature, and the name must match the central
    // directory's -- tools like unzip trust the local name).
    std::uint64_t dataOffset(std::size_t index) const;

    // For entries written with a data descriptor: the descriptor after the
    // data must carry the central directory's CRC and sizes. Our own
    // restore never reads it, but third-party tools do, and a zeroed one
    // is exactly the kind of "structurally fine, subtly wrong" tail the
    // crash-recovery rule must reject. True for entries without one.
    bool verifyDataDescriptor(std::size_t index) const;

    // Bytes the entry occupies in the file: local header + data + data
    // descriptor. Sums to `centralDirectoryOffset` minus dead space.
    std::uint64_t footprint(std::size_t index) const;

    void readEntry(std::size_t index, const std::function<void(std::span<const std::byte>)> &sink,
                   std::size_t chunkSize = 1u << 20) const;
    std::string readEntryToString(std::size_t index) const;

    // Streams the entry and compares its CRC32 with the central directory.
    bool verifyCrc(std::size_t index) const;

private:
    Zip64Reader(const ArchiveFile &file, ArchiveLayout layout, std::vector<CentralEntry> entries);

    const ArchiveFile *m_file;
    ArchiveLayout m_layout;
    std::vector<CentralEntry> m_entries;
    std::unordered_map<std::string, std::size_t> m_indexByName;
    mutable std::vector<std::optional<std::uint64_t>> m_dataOffsets;
};

}  // namespace seabass::infrastructure::stick_backup
