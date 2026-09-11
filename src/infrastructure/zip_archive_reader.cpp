// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/zip_archive_reader.hpp"

#include <zlib.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace seabass::infrastructure
{

namespace fs = std::filesystem;

namespace
{

constexpr std::uint32_t EndOfCentralDirectorySignature = 0x06054b50u;
constexpr std::uint32_t CentralDirectorySignature = 0x02014b50u;
constexpr std::uint32_t LocalHeaderSignature = 0x04034b50u;
constexpr std::uint16_t MethodStored = 0;
constexpr std::uint16_t MethodDeflate = 8;

std::uint16_t readU16(const std::string &bytes, size_t at)
{
    if (at + 2 > bytes.size()) {
        throw std::runtime_error("zip: truncated");
    }
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[at]))
        | static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[at + 1]) << 8);
}

std::uint32_t readU32(const std::string &bytes, size_t at)
{
    if (at + 4 > bytes.size()) {
        throw std::runtime_error("zip: truncated");
    }
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at]))
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 16)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3])) << 24);
}

// The record is at the very end unless the archive carries a comment, so
// scan backwards for its signature rather than assuming a fixed offset.
size_t findEndOfCentralDirectory(const std::string &bytes)
{
    if (bytes.size() < 22) {
        throw std::runtime_error("zip: too small to be an archive");
    }
    for (size_t at = bytes.size() - 22; ; --at) {
        if (readU32(bytes, at) == EndOfCentralDirectorySignature) {
            return at;
        }
        if (at == 0) {
            break;
        }
    }
    throw std::runtime_error("zip: no end-of-central-directory record");
}

std::string inflateRaw(const std::string &compressed, std::uint32_t expandedSize)
{
    std::string out(expandedSize, '\0');
    if (expandedSize == 0) {
        return out;
    }
    z_stream stream{};
    // Negative window bits: the entries hold a raw deflate stream with no
    // zlib header, which is what the writer produces.
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        throw std::runtime_error("zip: could not start decompression");
    }
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = reinterpret_cast<Bytef *>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    const int rc = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (rc != Z_STREAM_END) {
        throw std::runtime_error("zip: decompression failed");
    }
    return out;
}

// An entry named "../../etc/whatever" must not be written outside destDir.
// A corpus set can come from anywhere, so this is checked rather than
// assumed.
fs::path safeTargetFor(const fs::path &destDir, const std::string &entryName)
{
    if (entryName.empty() || entryName.front() == '/' || entryName.find(':') != std::string::npos) {
        throw std::runtime_error("zip: refusing absolute entry name \"" + entryName + "\"");
    }
    fs::path target = destDir;
    for (const auto &part : fs::path(entryName)) {
        const std::string piece = part.string();
        if (piece == "..") {
            throw std::runtime_error("zip: refusing entry name that escapes the destination: \"" + entryName + "\"");
        }
        if (piece == "." || piece.empty()) {
            continue;
        }
        target /= piece;
    }
    return target;
}

}  // namespace

void extractZipArchive(const fs::path &zipPath, const fs::path &destDir)
{
    std::ifstream in(zipPath, std::ios::binary);
    if (!in) {
        throw std::runtime_error("zip: could not open " + zipPath.string());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string bytes = buffer.str();

    const size_t eocd = findEndOfCentralDirectory(bytes);
    const std::uint16_t entryCount = readU16(bytes, eocd + 10);
    size_t cursor = readU32(bytes, eocd + 16);

    fs::create_directories(destDir);
    for (std::uint16_t i = 0; i < entryCount; ++i) {
        if (readU32(bytes, cursor) != CentralDirectorySignature) {
            throw std::runtime_error("zip: central directory entry " + std::to_string(i) + " is malformed");
        }
        const std::uint16_t method = readU16(bytes, cursor + 10);
        const std::uint32_t compressedSize = readU32(bytes, cursor + 20);
        const std::uint32_t expandedSize = readU32(bytes, cursor + 24);
        const std::uint16_t nameLength = readU16(bytes, cursor + 28);
        const std::uint16_t extraLength = readU16(bytes, cursor + 30);
        const std::uint16_t commentLength = readU16(bytes, cursor + 32);
        const std::uint32_t localOffset = readU32(bytes, cursor + 42);
        const std::string name = bytes.substr(cursor + 46, nameLength);
        cursor += 46 + nameLength + extraLength + commentLength;

        if (readU32(bytes, localOffset) != LocalHeaderSignature) {
            throw std::runtime_error("zip: local header for \"" + name + "\" is malformed");
        }
        const std::uint16_t localNameLength = readU16(bytes, localOffset + 26);
        const std::uint16_t localExtraLength = readU16(bytes, localOffset + 28);
        const size_t dataAt = localOffset + 30 + localNameLength + localExtraLength;
        if (dataAt + compressedSize > bytes.size()) {
            throw std::runtime_error("zip: data for \"" + name + "\" runs past the end of the archive");
        }
        const std::string data = bytes.substr(dataAt, compressedSize);

        std::string content;
        if (method == MethodStored) {
            content = data;
        } else if (method == MethodDeflate) {
            content = inflateRaw(data, expandedSize);
        } else {
            throw std::runtime_error("zip: \"" + name + "\" uses an unsupported compression method");
        }

        const fs::path target = safeTargetFor(destDir, name);
        fs::create_directories(target.parent_path());
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("zip: could not write " + target.string());
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
}

}  // namespace seabass::infrastructure
