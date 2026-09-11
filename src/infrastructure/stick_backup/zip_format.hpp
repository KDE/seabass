// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace seabass::infrastructure::stick_backup
{

// A structurally invalid archive (bad signature, offset past EOF, count
// mismatch, ...). Distinct from ArchiveIoError so callers can tell "the
// medium failed" from "the bytes are wrong".
class ArchiveFormatError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// The subset of the ZIP format (APPNOTE 6.3) the stick backup uses.
// Everything is little-endian on disk regardless of host, hence the
// explicit byte helpers rather than struct dumps.
namespace zip
{

constexpr std::uint32_t LocalFileHeaderSignature = 0x04034b50;
constexpr std::uint32_t DataDescriptorSignature = 0x08074b50;
constexpr std::uint32_t CentralDirectorySignature = 0x02014b50;
constexpr std::uint32_t Zip64EndOfCentralDirectorySignature = 0x06064b50;
constexpr std::uint32_t Zip64LocatorSignature = 0x07064b50;
constexpr std::uint32_t EndOfCentralDirectorySignature = 0x06054b50;

constexpr std::uint16_t Zip64ExtraId = 0x0001;
constexpr std::uint16_t ExtendedTimestampExtraId = 0x5455;

constexpr std::uint16_t MethodStore = 0;
constexpr std::uint16_t MethodDeflate = 8;
constexpr std::uint16_t FlagDataDescriptor = 1u << 3;
constexpr std::uint16_t FlagUtf8Names = 1u << 11;

// 4.5 -- the format level ZIP64 requires. Host 3 = Unix, so the high
// 16 bits of the external attributes are read as a POSIX mode by
// tools that care (Python's zipfile uses it to spot directories).
constexpr std::uint16_t VersionNeeded = 45;
constexpr std::uint16_t VersionMadeBy = (3u << 8) | 45;

constexpr std::uint32_t Max32 = 0xFFFFFFFFu;
constexpr std::uint16_t Max16 = 0xFFFFu;

constexpr std::size_t LocalFileHeaderSize = 30;
constexpr std::size_t CentralDirectoryEntrySize = 46;
constexpr std::size_t EndOfCentralDirectorySize = 22;
constexpr std::size_t Zip64LocatorSize = 20;
constexpr std::size_t Zip64EndOfCentralDirectorySize = 56;
// Our descriptors always carry 8-byte sizes (the local header always
// has a ZIP64 extra field, which is what tells a reader to expect them).
constexpr std::size_t DataDescriptorSize = 4 + 4 + 8 + 8;
constexpr std::size_t MaxCommentLength = 0xFFFF;

inline void putU16(std::string &out, std::uint16_t v)
{
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
}

inline void putU32(std::string &out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    }
}

inline void putU64(std::string &out, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    }
}

inline void requireRange(std::span<const std::byte> bytes, std::size_t offset, std::size_t length, const char *what)
{
    if (offset > bytes.size() || length > bytes.size() - offset) {
        throw ArchiveFormatError(std::string("truncated ") + what);
    }
}

inline std::uint16_t readU16(std::span<const std::byte> bytes, std::size_t offset, const char *what = "field")
{
    requireRange(bytes, offset, 2, what);
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[offset])
                                      | (std::to_integer<unsigned>(bytes[offset + 1]) << 8));
}

inline std::uint32_t readU32(std::span<const std::byte> bytes, std::size_t offset, const char *what = "field")
{
    requireRange(bytes, offset, 4, what);
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) {
        v = (v << 8) | std::to_integer<std::uint32_t>(bytes[offset + static_cast<std::size_t>(i)]);
    }
    return v;
}

inline std::uint64_t readU64(std::span<const std::byte> bytes, std::size_t offset, const char *what = "field")
{
    requireRange(bytes, offset, 8, what);
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | std::to_integer<std::uint64_t>(bytes[offset + static_cast<std::size_t>(i)]);
    }
    return v;
}

inline std::span<const std::byte> bytesOf(std::string_view s)
{
    return std::as_bytes(std::span<const char>(s.data(), s.size()));
}

struct DosDateTime
{
    std::uint16_t date = 0;
    std::uint16_t time = 0;
};

// DOS date/time (2 s resolution, no zone) from a Unix timestamp, taken
// as UTC. Only a courtesy for tools that ignore the extended-timestamp
// extra field; the manifest holds the real 64-bit mtime.
DosDateTime dosDateTimeFromUnix(std::int64_t unixSeconds);
std::int64_t unixFromDosDateTime(DosDateTime value);

}  // namespace zip

}  // namespace seabass::infrastructure::stick_backup
