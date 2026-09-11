// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/zip_archive_writer.hpp"

#include <zlib.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace seabass::infrastructure
{

namespace fs = std::filesystem;

namespace
{

void putU16(std::string &out, std::uint16_t v)
{
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
}

void putU32(std::string &out, std::uint32_t v)
{
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
    out.push_back(static_cast<char>((v >> 16) & 0xff));
    out.push_back(static_cast<char>((v >> 24) & 0xff));
}

// DOS date/time packed format ZIP local/central headers use -- just the
// current moment; nothing in this codebase reads these back, and no
// caller needs the original file's own mtime preserved (every file
// here was just freshly written by the anonymizer moments earlier).
struct DosDateTime
{
    std::uint16_t date = 0;
    std::uint16_t time = 0;
};

DosDateTime currentDosDateTime()
{
    std::time_t t = std::time(nullptr);
    std::tm tmVal{};
#if defined(_WIN32)
    localtime_s(&tmVal, &t);
#else
    localtime_r(&t, &tmVal);
#endif
    DosDateTime result;
    int year = tmVal.tm_year + 1900;
    if (year < 1980) {
        year = 1980;  // ZIP's DOS date can't represent anything earlier.
    }
    result.date = static_cast<std::uint16_t>(((year - 1980) << 9) | ((tmVal.tm_mon + 1) << 5) | tmVal.tm_mday);
    result.time =
        static_cast<std::uint16_t>((tmVal.tm_hour << 11) | (tmVal.tm_min << 5) | (tmVal.tm_sec / 2));
    return result;
}

// Raw deflate (no zlib header/trailer -- windowBits negative selects
// this), the compressed-data format the ZIP entry format itself
// expects. infrastructure::compression's compress()/decompress() wraps
// zlib's higher-level compress2()/uncompress(), which always produce a
// zlib-framed stream, not usable here directly.
std::string rawDeflate(const std::string &input)
{
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("zlib deflateInit2 failed");
    }

    std::string output;
    output.resize(deflateBound(&stream, static_cast<uLong>(input.size())));

    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef *>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());

    int result = deflate(&stream, Z_FINISH);
    if (result != Z_STREAM_END) {
        deflateEnd(&stream);
        throw std::runtime_error("zlib deflate failed to finish the stream");
    }
    output.resize(output.size() - stream.avail_out);
    deflateEnd(&stream);
    return output;
}

std::string readWholeFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("could not open " + path.string() + " for reading");
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

// One archive entry's worth of bookkeeping the central directory needs,
// recorded as each entry is written so the central directory (written
// after every file) doesn't have to re-derive any of it.
struct EntryRecord
{
    std::string archiveName;  // forward-slash separated, ZIP's own convention regardless of host OS
    std::uint32_t crc32 = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t uncompressedSize = 0;
    std::uint32_t localHeaderOffset = 0;
    DosDateTime timestamp;
};

constexpr std::uint32_t LocalFileHeaderSignature = 0x04034b50;
constexpr std::uint32_t CentralDirectorySignature = 0x02014b50;
constexpr std::uint32_t EndOfCentralDirectorySignature = 0x06054b50;
constexpr std::uint16_t VersionNeeded = 20;  // 2.0 -- the format level deflate + long filenames requires

void writeLocalFileHeader(std::ofstream &out, EntryRecord &entry)
{
    std::string header;
    putU32(header, LocalFileHeaderSignature);
    putU16(header, VersionNeeded);
    putU16(header, 0);  // general purpose flags
    putU16(header, Z_DEFLATED);
    putU16(header, entry.timestamp.time);
    putU16(header, entry.timestamp.date);
    putU32(header, entry.crc32);
    putU32(header, entry.compressedSize);
    putU32(header, entry.uncompressedSize);
    putU16(header, static_cast<std::uint16_t>(entry.archiveName.size()));
    putU16(header, 0);  // extra field length
    header += entry.archiveName;
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
}

void writeCentralDirectoryEntry(std::string &out, const EntryRecord &entry)
{
    putU32(out, CentralDirectorySignature);
    putU16(out, VersionNeeded);  // version made by
    putU16(out, VersionNeeded);  // version needed to extract
    putU16(out, 0);              // general purpose flags
    putU16(out, Z_DEFLATED);
    putU16(out, entry.timestamp.time);
    putU16(out, entry.timestamp.date);
    putU32(out, entry.crc32);
    putU32(out, entry.compressedSize);
    putU32(out, entry.uncompressedSize);
    putU16(out, static_cast<std::uint16_t>(entry.archiveName.size()));
    putU16(out, 0);  // extra field length
    putU16(out, 0);  // comment length
    putU16(out, 0);  // disk number start
    putU16(out, 0);  // internal file attributes
    putU32(out, 0);  // external file attributes
    putU32(out, entry.localHeaderOffset);
    out += entry.archiveName;
}

std::string toArchiveName(const fs::path &relative)
{
    std::string name = relative.generic_string();  // forward slashes, matches ZIP's own convention
    return name;
}

}  // namespace

void writeZipArchive(const fs::path &sourceDir, const fs::path &zipPath)
{
    if (!fs::exists(sourceDir) || !fs::is_directory(sourceDir)) {
        throw std::runtime_error("writeZipArchive: source directory does not exist: " + sourceDir.string());
    }

    std::vector<fs::path> files;
    for (const auto &entry : fs::recursive_directory_iterator(sourceDir)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    if (files.empty()) {
        throw std::runtime_error("writeZipArchive: source directory has no files: " + sourceDir.string());
    }

    std::ofstream out(zipPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("writeZipArchive: could not open " + zipPath.string() + " for writing");
    }

    DosDateTime timestamp = currentDosDateTime();
    std::vector<EntryRecord> entries;
    std::uint32_t offset = 0;

    for (const auto &filePath : files) {
        std::string content = readWholeFile(filePath);
        std::string compressed = rawDeflate(content);

        EntryRecord entry;
        entry.archiveName = toArchiveName(fs::relative(filePath, sourceDir));
        entry.crc32 = static_cast<std::uint32_t>(
            crc32(0L, reinterpret_cast<const Bytef *>(content.data()), static_cast<uInt>(content.size())));
        entry.compressedSize = static_cast<std::uint32_t>(compressed.size());
        entry.uncompressedSize = static_cast<std::uint32_t>(content.size());
        entry.localHeaderOffset = offset;
        entry.timestamp = timestamp;

        writeLocalFileHeader(out, entry);
        out.write(compressed.data(), static_cast<std::streamsize>(compressed.size()));

        offset += static_cast<std::uint32_t>(30 + entry.archiveName.size() + compressed.size());
        entries.push_back(std::move(entry));
    }

    std::uint32_t centralDirectoryOffset = offset;
    std::string centralDirectory;
    for (const auto &entry : entries) {
        writeCentralDirectoryEntry(centralDirectory, entry);
    }
    out.write(centralDirectory.data(), static_cast<std::streamsize>(centralDirectory.size()));

    std::string eocd;
    putU32(eocd, EndOfCentralDirectorySignature);
    putU16(eocd, 0);  // disk number
    putU16(eocd, 0);  // disk with central directory
    putU16(eocd, static_cast<std::uint16_t>(entries.size()));  // entries on this disk
    putU16(eocd, static_cast<std::uint16_t>(entries.size()));  // total entries
    putU32(eocd, static_cast<std::uint32_t>(centralDirectory.size()));
    putU32(eocd, centralDirectoryOffset);
    putU16(eocd, 0);  // comment length
    out.write(eocd.data(), static_cast<std::streamsize>(eocd.size()));

    if (!out) {
        throw std::runtime_error("writeZipArchive: write failed for " + zipPath.string());
    }
}

}  // namespace seabass::infrastructure
