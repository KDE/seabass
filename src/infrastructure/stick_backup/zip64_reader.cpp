// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/stick_backup/zip64_reader.hpp"

#include <zlib.h>

#include <algorithm>
#include <utility>

#include "infrastructure/stick_backup/zip_format.hpp"

namespace seabass::infrastructure::stick_backup
{

using namespace zip;

namespace
{

std::vector<std::byte> readRange(const ArchiveFile &file, std::uint64_t offset, std::size_t length)
{
    std::vector<std::byte> out(length);
    file.readAt(offset, out);
    return out;
}

struct ParsedExtras
{
    std::optional<std::uint64_t> uncompressedSize;
    std::optional<std::uint64_t> compressedSize;
    std::optional<std::uint64_t> localHeaderOffset;
    std::optional<std::int64_t> mtimeUnix;
};

// Walks the extra-field blocks of one central-directory entry, insisting
// on exactly the shape Zip64Writer produces: an optional ZIP64 block
// (present iff a 32-bit field saturated, carrying exactly the saturated
// fields in order) followed by the extended-timestamp block with just the
// mtime. Anything else is damage.
ParsedExtras parseExtras(std::span<const std::byte> extra, bool expectSize, bool expectOffset, const std::string &name)
{
    ParsedExtras result;
    std::size_t pos = 0;
    bool sawZip64 = false;
    bool sawTimestamp = false;
    while (pos < extra.size()) {
        std::uint16_t id = readU16(extra, pos, "extra field header");
        std::uint16_t length = readU16(extra, pos + 2, "extra field header");
        pos += 4;
        requireRange(extra, pos, length, "extra field body");
        std::span<const std::byte> body = extra.subspan(pos, length);
        if (id == Zip64ExtraId) {
            if (sawZip64 || sawTimestamp || (!expectSize && !expectOffset)) {
                throw ArchiveFormatError("unexpected zip64 extra field in " + name);
            }
            sawZip64 = true;
            std::size_t at = 0;
            if (expectSize) {
                result.uncompressedSize = readU64(body, at, "zip64 uncompressed size");
                result.compressedSize = readU64(body, at + 8, "zip64 compressed size");
                at += 16;
            }
            if (expectOffset) {
                result.localHeaderOffset = readU64(body, at, "zip64 local header offset");
                at += 8;
            }
            if (at != length) {
                throw ArchiveFormatError("zip64 extra field has the wrong length in " + name);
            }
        } else if (id == ExtendedTimestampExtraId) {
            if (sawTimestamp || length != 5 || std::to_integer<unsigned>(body[0]) != 0x01) {
                throw ArchiveFormatError("malformed extended timestamp in " + name);
            }
            sawTimestamp = true;
            result.mtimeUnix = static_cast<std::int32_t>(readU32(body, 1, "extended timestamp"));
        } else {
            throw ArchiveFormatError("unknown extra field in " + name);
        }
        pos += length;
    }
    if ((expectSize || expectOffset) && !sawZip64) {
        throw ArchiveFormatError("zip64 marker without a zip64 extra field in " + name);
    }
    if (!sawTimestamp) {
        throw ArchiveFormatError("missing extended timestamp in " + name);
    }
    return result;
}

}  // namespace

Zip64Reader::Zip64Reader(const ArchiveFile &file, ArchiveLayout layout, std::vector<CentralEntry> entries)
    : m_file(&file), m_layout(std::move(layout)), m_entries(std::move(entries)), m_dataOffsets(m_entries.size())
{
    m_indexByName.reserve(m_entries.size());
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        m_indexByName.emplace(m_entries[i].name, i);
    }
}

std::optional<Zip64Reader> Zip64Reader::tryOpen(const ArchiveFile &file, std::string *error)
{
    try {
        return open(file);
    } catch (const ArchiveFormatError &e) {
        if (error != nullptr) {
            *error = e.what();
        }
        return std::nullopt;
    }
}

Zip64Reader Zip64Reader::open(const ArchiveFile &file)
{
    const std::uint64_t fileSize = file.size();
    if (fileSize < EndOfCentralDirectorySize) {
        throw ArchiveFormatError("file too small to be an archive");
    }

    // The EOCD is the last thing in the file; we never write a comment,
    // so it must end exactly at EOF. Tolerate a comment (scan backward)
    // only as far as the format allows, and insist the comment length
    // agrees with the distance to EOF -- that is what rejects a stray
    // signature inside entry data.
    const std::size_t tailLength =
        static_cast<std::size_t>(std::min<std::uint64_t>(fileSize, EndOfCentralDirectorySize + MaxCommentLength));
    const std::uint64_t tailStart = fileSize - tailLength;
    std::vector<std::byte> tail = readRange(file, tailStart, tailLength);

    std::optional<std::size_t> eocdInTail;
    for (std::size_t i = tailLength - EndOfCentralDirectorySize + 1; i-- > 0;) {
        if (readU32(tail, i) == EndOfCentralDirectorySignature
            && readU16(tail, i + 20) == tailLength - (i + EndOfCentralDirectorySize)) {
            eocdInTail = i;
            break;
        }
    }
    if (!eocdInTail) {
        throw ArchiveFormatError("no end-of-central-directory record");
    }
    ArchiveLayout layout;
    layout.fileSize = fileSize;
    layout.endOfCentralDirectoryOffset = tailStart + *eocdInTail;

    std::uint64_t entryCount = 0;
    std::uint64_t centralDirectorySize = 0;
    std::uint64_t centralDirectoryOffset = 0;
    if (readU16(tail, *eocdInTail + 4) != 0 || readU16(tail, *eocdInTail + 6) != 0) {
        throw ArchiveFormatError("multi-disk archives are not supported");
    }

    // Zip64Writer always emits the zip64 record and locator, so their
    // absence is damage, not a legacy layout -- no fallback to the 16/32-
    // bit EOCD fields. (A zeroed locator would otherwise read as a valid,
    // slightly different archive, which the crash-recovery rule must
    // never accept.)
    if (layout.endOfCentralDirectoryOffset < Zip64LocatorSize + Zip64EndOfCentralDirectorySize) {
        throw ArchiveFormatError("no room for a zip64 end-of-central-directory record");
    }
    std::vector<std::byte> locator = readRange(file, layout.endOfCentralDirectoryOffset - Zip64LocatorSize, Zip64LocatorSize);
    if (readU32(locator, 0) != Zip64LocatorSignature) {
        throw ArchiveFormatError("missing zip64 end-of-central-directory locator");
    }
    layout.zip64LocatorOffset = layout.endOfCentralDirectoryOffset - Zip64LocatorSize;
    if (readU32(locator, 4) != 0 || readU32(locator, 16) != 1) {
        throw ArchiveFormatError("multi-disk zip64 archives are not supported");
    }
    std::uint64_t zip64EocdOffset = readU64(locator, 8);
    if (zip64EocdOffset + Zip64EndOfCentralDirectorySize > layout.zip64LocatorOffset) {
        throw ArchiveFormatError("zip64 end-of-central-directory offset out of range");
    }
    std::vector<std::byte> zip64Eocd = readRange(file, zip64EocdOffset, Zip64EndOfCentralDirectorySize);
    if (readU32(zip64Eocd, 0) != Zip64EndOfCentralDirectorySignature) {
        throw ArchiveFormatError("bad zip64 end-of-central-directory signature");
    }
    if (readU32(zip64Eocd, 16) != 0 || readU32(zip64Eocd, 20) != 0) {
        throw ArchiveFormatError("multi-disk zip64 archives are not supported");
    }
    if (readU64(zip64Eocd, 4) != Zip64EndOfCentralDirectorySize - 12 || readU16(zip64Eocd, 12) != VersionMadeBy
        || readU16(zip64Eocd, 14) != VersionNeeded || readU64(zip64Eocd, 24) != readU64(zip64Eocd, 32)) {
        throw ArchiveFormatError("zip64 end-of-central-directory record is malformed");
    }
    layout.zip64EndOfCentralDirectoryOffset = zip64EocdOffset;
    entryCount = readU64(zip64Eocd, 32);
    centralDirectorySize = readU64(zip64Eocd, 40);
    centralDirectoryOffset = readU64(zip64Eocd, 48);
    // The 16/32-bit EOCD copies must agree with the zip64 record (or be
    // saturated markers) -- disagreement means one of them was damaged.
    std::uint64_t eocdCountOnDisk = readU16(tail, *eocdInTail + 8);
    std::uint64_t eocdCount = readU16(tail, *eocdInTail + 10);
    std::uint64_t eocdSize = readU32(tail, *eocdInTail + 12);
    std::uint64_t eocdOffset = readU32(tail, *eocdInTail + 16);
    if (eocdCountOnDisk != eocdCount || (eocdCount != Max16 && eocdCount != entryCount)
        || (eocdSize != Max32 && eocdSize != centralDirectorySize) || (eocdOffset != Max32 && eocdOffset != centralDirectoryOffset)) {
        throw ArchiveFormatError("end-of-central-directory record disagrees with the zip64 record");
    }

    const std::uint64_t centralDirectoryEnd = layout.zip64EndOfCentralDirectoryOffset;
    if (centralDirectoryOffset > centralDirectoryEnd || centralDirectorySize > centralDirectoryEnd - centralDirectoryOffset) {
        throw ArchiveFormatError("central directory out of range");
    }
    if (centralDirectorySize < entryCount * CentralDirectoryEntrySize) {
        throw ArchiveFormatError("central directory too small for its entry count");
    }
    layout.centralDirectoryOffset = centralDirectoryOffset;
    layout.centralDirectorySize = centralDirectorySize;
    layout.entryCount = entryCount;

    std::vector<std::byte> directory = readRange(file, centralDirectoryOffset, static_cast<std::size_t>(centralDirectorySize));
    std::vector<CentralEntry> entries;
    entries.reserve(static_cast<std::size_t>(entryCount));
    std::size_t pos = 0;
    for (std::uint64_t n = 0; n < entryCount; ++n) {
        if (readU32(directory, pos, "central directory entry") != CentralDirectorySignature) {
            throw ArchiveFormatError("bad central directory entry signature");
        }
        std::uint16_t versionMadeBy = readU16(directory, pos + 4);
        std::uint16_t versionNeeded = readU16(directory, pos + 6);
        std::uint16_t flags = readU16(directory, pos + 8);
        std::uint16_t method = readU16(directory, pos + 10);
        DosDateTime stamp{readU16(directory, pos + 14), readU16(directory, pos + 12)};
        std::uint32_t crc = readU32(directory, pos + 16);
        std::uint32_t compressed32 = readU32(directory, pos + 20);
        std::uint32_t uncompressed32 = readU32(directory, pos + 24);
        std::uint16_t nameLength = readU16(directory, pos + 28);
        std::uint16_t extraLength = readU16(directory, pos + 30);
        std::uint16_t commentLength = readU16(directory, pos + 32);
        std::uint16_t diskNumberStart = readU16(directory, pos + 34);
        std::uint16_t internalAttributes = readU16(directory, pos + 36);
        std::uint32_t externalAttributes = readU32(directory, pos + 38);
        std::uint32_t offset32 = readU32(directory, pos + 42);
        pos += CentralDirectoryEntrySize;

        requireRange(directory, pos, static_cast<std::size_t>(nameLength) + extraLength + commentLength, "central directory entry");
        std::string name(reinterpret_cast<const char *>(directory.data() + pos), nameLength);
        std::span<const std::byte> extra(directory.data() + pos + nameLength, extraLength);
        pos += static_cast<std::size_t>(nameLength) + extraLength + commentLength;

        if (name.empty()) {
            throw ArchiveFormatError("central directory entry with an empty name");
        }
        // Every field Zip64Writer sets to a constant must read back as
        // that constant -- there is no legitimate reason for any of these
        // to differ, so a difference is damage.
        if (versionMadeBy != VersionMadeBy || versionNeeded != VersionNeeded
            || (method != MethodStore && method != MethodDeflate) || commentLength != 0
            || diskNumberStart != 0 || internalAttributes != 0 || (flags & ~(FlagUtf8Names | FlagDataDescriptor)) != 0
            || (flags & FlagUtf8Names) == 0) {
            throw ArchiveFormatError("central directory entry has unexpected header fields: " + name);
        }
        const bool expectSize = uncompressed32 == Max32 || compressed32 == Max32;
        ParsedExtras extras = parseExtras(extra, expectSize, offset32 == Max32, name);

        CentralEntry entry;
        entry.name = std::move(name);
        entry.method = method;
        entry.size = expectSize ? extras.uncompressedSize.value_or(Max32) : uncompressed32;
        entry.compressedSize = expectSize ? extras.compressedSize.value_or(Max32) : compressed32;
        if (entry.method == MethodStore && entry.compressedSize != entry.size) {
            throw ArchiveFormatError("stored entry with mismatched sizes: " + entry.name);
        }
        entry.localHeaderOffset = offset32 == Max32 ? extras.localHeaderOffset.value_or(Max32) : offset32;
        if (entry.localHeaderOffset + LocalFileHeaderSize > centralDirectoryOffset) {
            throw ArchiveFormatError("entry offset past the central directory: " + entry.name);
        }
        entry.crc32 = crc;
        entry.mtimeUnix = *extras.mtimeUnix;
        DosDateTime expectedStamp = dosDateTimeFromUnix(entry.mtimeUnix);
        if (stamp.date != expectedStamp.date || stamp.time != expectedStamp.time) {
            throw ArchiveFormatError("DOS timestamp disagrees with the extended timestamp in " + entry.name);
        }
        entry.isDirectory = entry.name.back() == '/';
        entry.hasDataDescriptor = (flags & FlagDataDescriptor) != 0;
        const std::uint32_t expectedAttributes = entry.isDirectory ? ((0040755u << 16) | 0x10u) : (0100644u << 16);
        if (externalAttributes != expectedAttributes || entry.hasDataDescriptor == entry.isDirectory
            || (entry.isDirectory && (entry.size != 0 || entry.crc32 != 0))) {
            throw ArchiveFormatError("central directory entry attributes are inconsistent: " + entry.name);
        }
        entries.push_back(std::move(entry));
    }
    if (pos != centralDirectorySize) {
        throw ArchiveFormatError("central directory size does not match its entries");
    }

    return Zip64Reader(file, layout, std::move(entries));
}

std::optional<std::size_t> Zip64Reader::findEntry(std::string_view name) const
{
    auto it = m_indexByName.find(std::string(name));
    if (it == m_indexByName.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::uint64_t Zip64Reader::dataOffset(std::size_t index) const
{
    const CentralEntry &entry = m_entries.at(index);
    if (m_dataOffsets[index]) {
        return *m_dataOffsets[index];
    }
    std::vector<std::byte> header = readRange(*m_file, entry.localHeaderOffset, LocalFileHeaderSize);
    if (readU32(header, 0) != LocalFileHeaderSignature) {
        throw ArchiveFormatError("bad local header signature for " + entry.name);
    }
    const std::uint16_t expectedFlags =
        static_cast<std::uint16_t>(FlagUtf8Names | (entry.hasDataDescriptor ? FlagDataDescriptor : 0));
    // Local headers carry zeros for CRC and sizes (data descriptor) and a
    // fixed extra-field layout: 16-byte zip64 placeholder + 5-byte
    // timestamp for files, timestamp only for directories.
    const std::uint16_t expectedExtraLength = entry.isDirectory ? 9 : 29;
    DosDateTime stamp{readU16(header, 12), readU16(header, 10)};
    DosDateTime expectedStamp = dosDateTimeFromUnix(entry.mtimeUnix);
    // The local header must agree with the central directory, method
    // included -- a mismatch there is how a reader is tricked into
    // interpreting bytes one way while a tool interprets them another.
    if (readU16(header, 4) != VersionNeeded || readU16(header, 6) != expectedFlags || readU16(header, 8) != entry.method
        || readU32(header, 14) != 0 || readU32(header, 18) != 0 || readU32(header, 22) != 0
        || readU16(header, 28) != expectedExtraLength || stamp.date != expectedStamp.date || stamp.time != expectedStamp.time) {
        throw ArchiveFormatError("local header fields disagree with the central directory for " + entry.name);
    }
    std::uint16_t nameLength = readU16(header, 26);
    std::uint16_t extraLength = readU16(header, 28);
    std::uint64_t offset = entry.localHeaderOffset + LocalFileHeaderSize + nameLength + extraLength;
    if (offset > m_layout.centralDirectoryOffset || entry.compressedSize > m_layout.centralDirectoryOffset - offset) {
        throw ArchiveFormatError("entry data runs into the central directory: " + entry.name);
    }
    if (nameLength != entry.name.size()) {
        throw ArchiveFormatError("local header name length differs from the central directory for " + entry.name);
    }
    std::vector<std::byte> localName = readRange(*m_file, entry.localHeaderOffset + LocalFileHeaderSize, nameLength);
    if (std::string_view(reinterpret_cast<const char *>(localName.data()), localName.size()) != entry.name) {
        throw ArchiveFormatError("local header name differs from the central directory for " + entry.name);
    }
    m_dataOffsets[index] = offset;
    return offset;
}

bool Zip64Reader::verifyDataDescriptor(std::size_t index) const
{
    const CentralEntry &entry = m_entries.at(index);
    if (!entry.hasDataDescriptor) {
        return true;
    }
    std::uint64_t at = dataOffset(index) + entry.compressedSize;
    if (at + DataDescriptorSize > m_layout.centralDirectoryOffset) {
        return false;
    }
    std::vector<std::byte> descriptor = readRange(*m_file, at, DataDescriptorSize);
    return readU32(descriptor, 0) == DataDescriptorSignature && readU32(descriptor, 4) == entry.crc32
           && readU64(descriptor, 8) == entry.compressedSize && readU64(descriptor, 16) == entry.size;
}

std::uint64_t Zip64Reader::footprint(std::size_t index) const
{
    const CentralEntry &entry = m_entries.at(index);
    std::uint64_t headerBytes = dataOffset(index) - entry.localHeaderOffset;
    return headerBytes + entry.compressedSize + (entry.hasDataDescriptor ? DataDescriptorSize : 0);
}

void Zip64Reader::readEntry(std::size_t index, const std::function<void(std::span<const std::byte>)> &sink,
                            std::size_t chunkSize) const
{
    const CentralEntry &entry = m_entries.at(index);
    std::uint64_t offset = dataOffset(index);
    std::uint64_t remaining = entry.compressedSize;
    std::vector<std::byte> buffer(
        static_cast<std::size_t>(std::min<std::uint64_t>(chunkSize, std::max<std::uint64_t>(remaining, 1))));

    if (entry.method == MethodStore) {
        while (remaining > 0) {
            std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            std::span<std::byte> piece(buffer.data(), take);
            m_file->readAt(offset, piece);
            sink(piece);
            offset += take;
            remaining -= take;
        }
        return;
    }

    // Deflated: the sink still sees the original bytes, so restore, the
    // CRC check and the manifest comparison all stay as they were.
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        throw ArchiveFormatError("could not start inflate for " + entry.name);
    }
    struct StreamGuard
    {
        z_stream *s;
        ~StreamGuard() { inflateEnd(s); }
    } guard{&stream};

    std::vector<std::byte> out(1u << 16);
    std::uint64_t produced = 0;
    bool ended = false;
    while (!ended) {
        if (remaining == 0 && stream.avail_in == 0) {
            throw ArchiveFormatError("deflated entry ended early: " + entry.name);
        }
        std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        if (take > 0) {
            m_file->readAt(offset, std::span<std::byte>(buffer.data(), take));
            offset += take;
            remaining -= take;
            stream.next_in = reinterpret_cast<Bytef *>(buffer.data());
            stream.avail_in = static_cast<uInt>(take);
        }
        do {
            stream.next_out = reinterpret_cast<Bytef *>(out.data());
            stream.avail_out = static_cast<uInt>(out.size());
            const int rc = inflate(&stream, Z_NO_FLUSH);
            if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
                throw ArchiveFormatError("corrupt deflate stream in " + entry.name);
            }
            const std::size_t got = out.size() - stream.avail_out;
            if (got > 0) {
                sink(std::span<const std::byte>(out.data(), got));
                produced += got;
            }
            if (rc == Z_STREAM_END) {
                ended = true;
                break;
            }
            if (rc == Z_BUF_ERROR && got == 0) {
                break;  // needs more input
            }
        } while (stream.avail_out == 0);
    }
    if (produced != entry.size) {
        throw ArchiveFormatError("deflated entry has the wrong length: " + entry.name);
    }
}

std::string Zip64Reader::readEntryToString(std::size_t index) const
{
    std::string out;
    out.reserve(static_cast<std::size_t>(m_entries.at(index).size));
    readEntry(index, [&out](std::span<const std::byte> piece) {
        out.append(reinterpret_cast<const char *>(piece.data()), piece.size());
    });
    return out;
}

bool Zip64Reader::verifyCrc(std::size_t index) const
{
    std::uint32_t crc = 0;
    readEntry(index, [&crc](std::span<const std::byte> piece) {
        crc = static_cast<std::uint32_t>(
            ::crc32(crc, reinterpret_cast<const Bytef *>(piece.data()), static_cast<uInt>(piece.size())));
    });
    return crc == m_entries.at(index).crc32;
}

}  // namespace seabass::infrastructure::stick_backup
