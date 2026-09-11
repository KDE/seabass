// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/stick_backup/archive_file.hpp"

namespace seabass::infrastructure::stick_backup
{

// One entry as the central directory describes it. Carried unchanged
// from one archive generation to the next by the incremental update
// (only offsets of *new* entries are ever computed).
// How an entry's bytes are stored. Deflate is worth it only where the
// content actually compresses: cue and database files do (about 30% off),
// audio does not, and paying deflate for an MP3 is pure CPU for nothing.
// So it is chosen per entry by the caller, never globally.
enum class Compression
{
    Store,
    Deflate,
};

struct CentralEntry
{
    std::string name;  // forward slashes; directories end with '/'
    std::uint64_t localHeaderOffset = 0;
    std::uint64_t size = 0;            // uncompressed
    std::uint64_t compressedSize = 0;  // equal to `size` when stored
    std::uint16_t method = 0;          // zip::MethodStore / zip::MethodDeflate
    std::uint32_t crc32 = 0;
    std::int64_t mtimeUnix = 0;
    bool isDirectory = false;
    bool hasDataDescriptor = false;
};

// Byte offsets of every structural boundary Zip64Writer::finish() laid
// down, in file order. The fault-injection tests truncate around each of
// these; the updater records some of them in the journal.
struct WriterBoundaries
{
    std::uint64_t manifestOffset = 0;
    std::uint64_t centralDirectoryOffset = 0;
    std::uint64_t zip64EndOfCentralDirectoryOffset = 0;
    std::uint64_t zip64LocatorOffset = 0;
    std::uint64_t endOfCentralDirectoryOffset = 0;
    std::uint64_t endOffset = 0;
};

// Streams a ZIP64 archive in STORE mode onto an ArchiveFile, append-only.
//
// Why data descriptors: a file's CRC (and, strictly, its size -- it may
// change between stat and read) is only known once its bytes have
// streamed past, and this writer may never seek back to patch a header.
// So every file entry sets general-purpose bit 3, writes zeros for
// CRC/sizes in the local header, and follows the data with a descriptor
// carrying the real values (8-byte sizes, announced by a ZIP64 extra
// field that is always present in the local header). This is exactly what
// Java's ZipOutputStream, Go's archive/zip and .NET do for streamed
// entries, so every mainstream reader handles it; the authoritative
// values live in the central directory anyway. Directory entries have
// nothing to stream and get plain headers.
//
// The writer never calls barrier(): where the durability barriers go is
// the incremental updater's decision (see archive_updater.hpp), and the
// fault-injection tests check that placement, not this class.
//
// The ZIP64 end-of-central-directory record and locator are always
// written, whether or not any field overflows -- one layout to test and
// to read back, and every tool that matters accepts it.
class Zip64Writer
{
public:
    // `carriedEntries` are already in the file (an incremental update
    // appends to an existing archive); they are listed in the new central
    // directory unchanged.
    Zip64Writer(ArchiveFile &file, std::vector<CentralEntry> carriedEntries);

    // Handle for streaming one file entry. Only one may be open at a time.
    // Destroying it without finish() abandons the entry: its bytes stay in
    // the file as dead space and it is not listed -- which is precisely the
    // "cancelled mid-file" outcome the design wants.
    class EntrySink
    {
    public:
        EntrySink(EntrySink &&other) noexcept;
        EntrySink &operator=(EntrySink &&) = delete;
        EntrySink(const EntrySink &) = delete;
        EntrySink &operator=(const EntrySink &) = delete;
        ~EntrySink();

        void write(std::span<const std::byte> bytes);
        // Uncompressed bytes handed in, which is what callers count.
        std::uint64_t bytesWritten() const { return m_bytesWritten; }
        std::uint64_t compressedBytesWritten() const { return m_compressedBytes; }

        // Writes the data descriptor, records the entry, returns it. The
        // SHA-256 of the streamed bytes is available afterwards for the
        // manifest.
        CentralEntry finish();
        const hashing::Sha256Digest &sha256() const { return m_sha256; }

    private:
        friend class Zip64Writer;
        EntrySink(Zip64Writer &writer, CentralEntry entry, Compression compression);
        void deflateChunk(std::span<const std::byte> bytes, bool finishStream);

        Zip64Writer *m_writer;
        CentralEntry m_entry;
        std::uint32_t m_crc = 0;
        hashing::Sha256 m_hasher;
        hashing::Sha256Digest m_sha256{};
        std::uint64_t m_bytesWritten = 0;
        std::uint64_t m_compressedBytes = 0;
        bool m_finished = false;
        // Held by pointer so the header does not drag zlib in.
        std::shared_ptr<void> m_deflater;
    };

    // Level 1 rather than 6 throughout: on real analysis files level 6
    // buys 1.2 MB out of a 26 MB saving for 2.5x the CPU. See
    // docs/write-path-performance.md, round 16.
    static constexpr int DeflateLevel = 1;

    EntrySink beginFile(std::string name, std::int64_t mtimeUnix, Compression compression = Compression::Store);
    CentralEntry addDirectory(std::string name, std::int64_t mtimeUnix);
    CentralEntry addFileFromMemory(std::string name, std::int64_t mtimeUnix, std::span<const std::byte> content,
                                   hashing::Sha256Digest *sha256Out = nullptr,
                                   Compression compression = Compression::Store);

    // Appends the manifest as the last entry, then the central directory
    // (carried + new entries), ZIP64 EOCD, locator and EOCD. The writer is
    // spent afterwards.
    WriterBoundaries finish(std::string_view manifestBytes, std::string manifestName, std::int64_t manifestMtimeUnix);

    // Drops the last `count` *new* entries from the list that finish()
    // will write. Their bytes stay in the file as dead space -- this is
    // how a torn database copy is retracted before the central directory
    // exists. Never reaches into carried entries.
    void forgetLastEntries(std::size_t count);

    // Carried entries followed by every entry added so far (excluding an
    // entry whose sink is still open).
    const std::vector<CentralEntry> &entries() const { return m_entries; }
    std::size_t newEntryCount() const { return m_entries.size() - m_carriedCount; }
    ArchiveFile &file() { return m_file; }

private:
    void requireNoOpenSink() const;
    void writeLocalHeader(const CentralEntry &entry, bool withDataDescriptor);
    static std::string centralDirectoryEntry(const CentralEntry &entry);

    ArchiveFile &m_file;
    std::vector<CentralEntry> m_entries;
    std::size_t m_carriedCount = 0;
    bool m_sinkOpen = false;
    bool m_finished = false;
};

}  // namespace seabass::infrastructure::stick_backup
