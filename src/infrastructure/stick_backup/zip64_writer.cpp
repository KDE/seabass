// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/stick_backup/zip64_writer.hpp"

#include <zlib.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "infrastructure/stick_backup/zip_format.hpp"

namespace seabass::infrastructure::stick_backup
{

using namespace zip;

namespace
{

std::string extendedTimestampExtra(std::int64_t mtimeUnix)
{
    // 0x5455 "UT": flags byte (bit 0 = mtime present) + int32 mtime. The
    // field is 32-bit by definition; clamp rather than wrap for dates the
    // format cannot hold. The manifest keeps the exact value.
    std::int64_t clamped =
        std::clamp<std::int64_t>(mtimeUnix, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max());
    std::string out;
    putU16(out, ExtendedTimestampExtraId);
    putU16(out, 5);
    out.push_back(static_cast<char>(0x01));
    putU32(out, static_cast<std::uint32_t>(static_cast<std::int32_t>(clamped)));
    return out;
}

namespace
{

// Owns one deflate stream for the life of an entry. Raw deflate (negative
// window bits): a zip entry carries no zlib wrapper of its own.
struct Deflater
{
    z_stream stream{};
    std::vector<std::byte> out;

    Deflater()
    {
        out.resize(1u << 16);
        const int rc = deflateInit2(&stream, Zip64Writer::DeflateLevel, Z_DEFLATED, -MAX_WBITS, 8,
                                    Z_DEFAULT_STRATEGY);
        if (rc != Z_OK) {
            throw ArchiveFormatError("could not start deflate");
        }
    }
    ~Deflater() { deflateEnd(&stream); }
    Deflater(const Deflater &) = delete;
    Deflater &operator=(const Deflater &) = delete;
};

}  // namespace

// Local-header ZIP64 extra: sizes are unknown at header time (they go in
// the data descriptor), but the field's presence is what tells readers
// the descriptor carries 8-byte sizes.
std::string localZip64Extra()
{
    std::string out;
    putU16(out, Zip64ExtraId);
    putU16(out, 16);
    putU64(out, 0);
    putU64(out, 0);
    return out;
}

void appendToFile(ArchiveFile &file, const std::string &bytes)
{
    file.append(bytesOf(bytes));
}

std::uint32_t crcUpdate(std::uint32_t crc, std::span<const std::byte> bytes)
{
    const auto *p = reinterpret_cast<const Bytef *>(bytes.data());
    std::size_t remaining = bytes.size();
    // zlib's crc32 takes a uInt length; feed it in bounded pieces.
    while (remaining > 0) {
        uInt piece = static_cast<uInt>(std::min<std::size_t>(remaining, 1u << 30));
        crc = static_cast<std::uint32_t>(::crc32(crc, p, piece));
        p += piece;
        remaining -= piece;
    }
    return crc;
}

}  // namespace

// ---- EntrySink ----

Zip64Writer::EntrySink::EntrySink(Zip64Writer &writer, CentralEntry entry, Compression compression)
    : m_writer(&writer), m_entry(std::move(entry))
{
    if (compression == Compression::Deflate) {
        m_deflater = std::make_shared<Deflater>();
    }
}

Zip64Writer::EntrySink::EntrySink(EntrySink &&other) noexcept
    : m_writer(other.m_writer), m_entry(std::move(other.m_entry)), m_crc(other.m_crc), m_hasher(other.m_hasher),
      m_sha256(other.m_sha256), m_bytesWritten(other.m_bytesWritten), m_compressedBytes(other.m_compressedBytes),
      m_finished(other.m_finished), m_deflater(std::move(other.m_deflater))
{
    other.m_writer = nullptr;
    other.m_finished = true;
}

Zip64Writer::EntrySink::~EntrySink()
{
    if (m_writer != nullptr && !m_finished) {
        // Abandoned: the bytes already appended stay as dead space, the
        // entry is never listed. Release the writer for the next entry.
        m_writer->m_sinkOpen = false;
    }
}

void Zip64Writer::EntrySink::write(std::span<const std::byte> bytes)
{
    if (m_finished || m_writer == nullptr) {
        throw std::logic_error("Zip64Writer::EntrySink::write after finish");
    }
    if (bytes.empty()) {
        return;
    }
    // CRC and SHA-256 are over the uncompressed bytes: the ZIP format
    // defines the CRC that way, and the manifest digest has to describe
    // the real content whatever it was stored as.
    m_crc = crcUpdate(m_crc, bytes);
    m_hasher.update(bytes);
    m_bytesWritten += bytes.size();

    if (m_deflater) {
        deflateChunk(bytes, false);
    } else {
        m_writer->m_file.append(bytes);
        m_compressedBytes += bytes.size();
    }
}

void Zip64Writer::EntrySink::deflateChunk(std::span<const std::byte> bytes, bool finishStream)
{
    auto &deflater = *static_cast<Deflater *>(m_deflater.get());
    deflater.stream.next_in =
        const_cast<Bytef *>(reinterpret_cast<const Bytef *>(bytes.data()));
    deflater.stream.avail_in = static_cast<uInt>(bytes.size());
    do {
        deflater.stream.next_out = reinterpret_cast<Bytef *>(deflater.out.data());
        deflater.stream.avail_out = static_cast<uInt>(deflater.out.size());
        const int rc = deflate(&deflater.stream, finishStream ? Z_FINISH : Z_NO_FLUSH);
        if (rc == Z_STREAM_ERROR) {
            throw ArchiveFormatError("deflate failed for " + m_entry.name);
        }
        const std::size_t produced = deflater.out.size() - deflater.stream.avail_out;
        if (produced > 0) {
            m_writer->m_file.append(std::span<const std::byte>(deflater.out.data(), produced));
            m_compressedBytes += produced;
        }
        if (finishStream && rc == Z_STREAM_END) {
            break;
        }
    } while (deflater.stream.avail_out == 0 || (finishStream && deflater.stream.avail_in > 0));
}

CentralEntry Zip64Writer::EntrySink::finish()
{
    if (m_finished || m_writer == nullptr) {
        throw std::logic_error("Zip64Writer::EntrySink::finish called twice");
    }
    if (m_deflater) {
        deflateChunk({}, true);
    }

    std::string descriptor;
    putU32(descriptor, DataDescriptorSignature);
    putU32(descriptor, m_crc);
    putU64(descriptor, m_compressedBytes);
    putU64(descriptor, m_bytesWritten);
    appendToFile(m_writer->m_file, descriptor);

    m_entry.size = m_bytesWritten;
    m_entry.compressedSize = m_compressedBytes;
    m_entry.crc32 = m_crc;
    m_entry.hasDataDescriptor = true;
    m_sha256 = m_hasher.finish();
    m_finished = true;
    m_writer->m_sinkOpen = false;
    m_writer->m_entries.push_back(m_entry);
    return m_entry;
}

// ---- Zip64Writer ----

Zip64Writer::Zip64Writer(ArchiveFile &file, std::vector<CentralEntry> carriedEntries)
    : m_file(file), m_entries(std::move(carriedEntries)), m_carriedCount(m_entries.size())
{
}

void Zip64Writer::requireNoOpenSink() const
{
    if (m_finished) {
        throw std::logic_error("Zip64Writer used after finish()");
    }
    if (m_sinkOpen) {
        throw std::logic_error("Zip64Writer: previous entry's sink is still open");
    }
}

void Zip64Writer::writeLocalHeader(const CentralEntry &entry, bool withDataDescriptor)
{
    if (entry.name.empty() || entry.name.size() > Max16) {
        throw ArchiveFormatError("entry name length out of range: " + entry.name.substr(0, 80));
    }
    std::string extra = extendedTimestampExtra(entry.mtimeUnix);
    if (withDataDescriptor) {
        extra = localZip64Extra() + extra;
    }
    DosDateTime stamp = dosDateTimeFromUnix(entry.mtimeUnix);

    std::string header;
    putU32(header, LocalFileHeaderSignature);
    putU16(header, VersionNeeded);
    putU16(header, static_cast<std::uint16_t>(FlagUtf8Names | (withDataDescriptor ? FlagDataDescriptor : 0)));
    putU16(header, entry.method);
    putU16(header, stamp.time);
    putU16(header, stamp.date);
    putU32(header, 0);  // crc -- descriptor
    putU32(header, 0);  // compressed size -- descriptor
    putU32(header, 0);  // uncompressed size -- descriptor
    putU16(header, static_cast<std::uint16_t>(entry.name.size()));
    putU16(header, static_cast<std::uint16_t>(extra.size()));
    header += entry.name;
    header += extra;
    appendToFile(m_file, header);
}

Zip64Writer::EntrySink Zip64Writer::beginFile(std::string name, std::int64_t mtimeUnix, Compression compression)
{
    requireNoOpenSink();
    CentralEntry entry;
    entry.name = std::move(name);
    entry.mtimeUnix = mtimeUnix;
    entry.method = compression == Compression::Deflate ? MethodDeflate : MethodStore;
    entry.localHeaderOffset = m_file.size();
    writeLocalHeader(entry, true);
    m_sinkOpen = true;
    return EntrySink(*this, std::move(entry), compression);
}

CentralEntry Zip64Writer::addDirectory(std::string name, std::int64_t mtimeUnix)
{
    requireNoOpenSink();
    if (name.empty() || name.back() != '/') {
        name.push_back('/');
    }
    CentralEntry entry;
    entry.name = std::move(name);
    entry.mtimeUnix = mtimeUnix;
    entry.isDirectory = true;
    entry.method = MethodStore;
    entry.localHeaderOffset = m_file.size();
    writeLocalHeader(entry, false);
    m_entries.push_back(entry);
    return entry;
}

CentralEntry Zip64Writer::addFileFromMemory(std::string name, std::int64_t mtimeUnix, std::span<const std::byte> content,
                                            hashing::Sha256Digest *sha256Out, Compression compression)
{
    EntrySink sink = beginFile(std::move(name), mtimeUnix, compression);
    sink.write(content);
    CentralEntry entry = sink.finish();
    if (sha256Out != nullptr) {
        *sha256Out = sink.sha256();
    }
    return entry;
}

void Zip64Writer::forgetLastEntries(std::size_t count)
{
    requireNoOpenSink();
    if (count > m_entries.size() - m_carriedCount) {
        throw std::logic_error("Zip64Writer::forgetLastEntries: more entries than were added");
    }
    m_entries.resize(m_entries.size() - count);
}

std::string Zip64Writer::centralDirectoryEntry(const CentralEntry &entry)
{
    const bool needSize = entry.size >= Max32 || entry.compressedSize >= Max32;
    const bool needOffset = entry.localHeaderOffset >= Max32;
    std::string zip64Extra;
    if (needSize || needOffset) {
        std::string fields;
        if (needSize) {
            putU64(fields, entry.size);            // uncompressed
            putU64(fields, entry.compressedSize);  // compressed
        }
        if (needOffset) {
            putU64(fields, entry.localHeaderOffset);
        }
        putU16(zip64Extra, Zip64ExtraId);
        putU16(zip64Extra, static_cast<std::uint16_t>(fields.size()));
        zip64Extra += fields;
    }
    std::string extra = zip64Extra + extendedTimestampExtra(entry.mtimeUnix);
    DosDateTime stamp = dosDateTimeFromUnix(entry.mtimeUnix);

    std::uint16_t flags = FlagUtf8Names;
    if (entry.hasDataDescriptor) {
        flags |= FlagDataDescriptor;
    }
    // Unix mode in the high half (regular 0644 / directory 0755), plus the
    // DOS directory bit so both families of tools agree on what is a dir.
    std::uint32_t externalAttributes =
        entry.isDirectory ? ((0040755u << 16) | 0x10u) : (0100644u << 16);

    std::string out;
    putU32(out, CentralDirectorySignature);
    putU16(out, VersionMadeBy);
    putU16(out, VersionNeeded);
    putU16(out, flags);
    putU16(out, entry.method);
    putU16(out, stamp.time);
    putU16(out, stamp.date);
    putU32(out, entry.crc32);
    putU32(out, needSize ? Max32 : static_cast<std::uint32_t>(entry.compressedSize));
    putU32(out, needSize ? Max32 : static_cast<std::uint32_t>(entry.size));
    putU16(out, static_cast<std::uint16_t>(entry.name.size()));
    putU16(out, static_cast<std::uint16_t>(extra.size()));
    putU16(out, 0);  // comment length
    putU16(out, 0);  // disk number start
    putU16(out, 0);  // internal attributes
    putU32(out, externalAttributes);
    putU32(out, needOffset ? Max32 : static_cast<std::uint32_t>(entry.localHeaderOffset));
    out += entry.name;
    out += extra;
    return out;
}

WriterBoundaries Zip64Writer::finish(std::string_view manifestBytes, std::string manifestName, std::int64_t manifestMtimeUnix)
{
    requireNoOpenSink();
    WriterBoundaries boundaries;

    boundaries.manifestOffset = m_file.size();
    addFileFromMemory(std::move(manifestName), manifestMtimeUnix, bytesOf(manifestBytes));

    boundaries.centralDirectoryOffset = m_file.size();
    std::string centralDirectory;
    for (const CentralEntry &entry : m_entries) {
        centralDirectory += centralDirectoryEntry(entry);
    }
    appendToFile(m_file, centralDirectory);
    const std::uint64_t centralDirectorySize = centralDirectory.size();
    const std::uint64_t entryCount = m_entries.size();

    boundaries.zip64EndOfCentralDirectoryOffset = m_file.size();
    std::string zip64Eocd;
    putU32(zip64Eocd, Zip64EndOfCentralDirectorySignature);
    putU64(zip64Eocd, Zip64EndOfCentralDirectorySize - 12);  // size of the remainder of this record
    putU16(zip64Eocd, VersionMadeBy);
    putU16(zip64Eocd, VersionNeeded);
    putU32(zip64Eocd, 0);  // this disk
    putU32(zip64Eocd, 0);  // disk with the central directory
    putU64(zip64Eocd, entryCount);
    putU64(zip64Eocd, entryCount);
    putU64(zip64Eocd, centralDirectorySize);
    putU64(zip64Eocd, boundaries.centralDirectoryOffset);
    appendToFile(m_file, zip64Eocd);

    boundaries.zip64LocatorOffset = m_file.size();
    std::string locator;
    putU32(locator, Zip64LocatorSignature);
    putU32(locator, 0);  // disk with the zip64 EOCD
    putU64(locator, boundaries.zip64EndOfCentralDirectoryOffset);
    putU32(locator, 1);  // total disks
    appendToFile(m_file, locator);

    boundaries.endOfCentralDirectoryOffset = m_file.size();
    std::string eocd;
    putU32(eocd, EndOfCentralDirectorySignature);
    putU16(eocd, 0);  // this disk
    putU16(eocd, 0);  // disk with the central directory
    putU16(eocd, entryCount >= Max16 ? Max16 : static_cast<std::uint16_t>(entryCount));
    putU16(eocd, entryCount >= Max16 ? Max16 : static_cast<std::uint16_t>(entryCount));
    putU32(eocd, centralDirectorySize >= Max32 ? Max32 : static_cast<std::uint32_t>(centralDirectorySize));
    putU32(eocd, boundaries.centralDirectoryOffset >= Max32 ? Max32
                                                              : static_cast<std::uint32_t>(boundaries.centralDirectoryOffset));
    putU16(eocd, 0);  // comment length
    appendToFile(m_file, eocd);

    boundaries.endOffset = m_file.size();
    m_finished = true;
    return boundaries;
}

}  // namespace seabass::infrastructure::stick_backup
