// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <zlib.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "infrastructure/zip_archive_reader.hpp"
#include "infrastructure/zip_archive_writer.hpp"

#include "scratch_path.hpp"

using seabass::infrastructure::writeZipArchive;
namespace fs = std::filesystem;

namespace
{

void touch(const fs::path &path, const std::string &content)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

std::string readWholeFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

std::uint16_t readU16(const std::string &buf, size_t offset)
{
    return static_cast<std::uint16_t>(static_cast<unsigned char>(buf[offset]) |
                                       (static_cast<unsigned char>(buf[offset + 1]) << 8));
}

std::uint32_t readU32(const std::string &buf, size_t offset)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(buf[offset])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(buf[offset + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(buf[offset + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(buf[offset + 3])) << 24);
}

std::string rawInflate(const std::string &compressed, std::size_t originalSize)
{
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        throw std::runtime_error("inflateInit2 failed");
    }
    std::string output(originalSize, '\0');
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = reinterpret_cast<Bytef *>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    int result = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (result != Z_STREAM_END) {
        throw std::runtime_error("inflate did not finish the stream");
    }
    return output;
}

// Parses just enough of the archive (EOCD -> central directory ->
// local headers) to verify writeZipArchive() produced a structurally
// correct, round-trippable file -- deliberately independent of the
// writer's own internals, so this actually exercises the on-disk
// format rather than re-testing the same code path.
struct ParsedEntry
{
    std::string name;
    std::string content;
};

std::vector<ParsedEntry> parseZip(const fs::path &zipPath)
{
    std::string bytes = readWholeFile(zipPath);
    assert(bytes.size() >= 22);

    // EOCD is the last 22 bytes here (no archive comment ever written).
    size_t eocdOffset = bytes.size() - 22;
    assert(readU32(bytes, eocdOffset) == 0x06054b50u);
    std::uint16_t entryCount = readU16(bytes, eocdOffset + 10);
    std::uint32_t centralDirOffset = readU32(bytes, eocdOffset + 16);

    std::vector<ParsedEntry> entries;
    size_t cdCursor = centralDirOffset;
    for (int i = 0; i < entryCount; i++) {
        assert(readU32(bytes, cdCursor) == 0x02014b50u);
        std::uint32_t crc = readU32(bytes, cdCursor + 16);
        std::uint32_t compressedSize = readU32(bytes, cdCursor + 20);
        std::uint32_t uncompressedSize = readU32(bytes, cdCursor + 24);
        std::uint16_t nameLen = readU16(bytes, cdCursor + 28);
        std::uint32_t localOffset = readU32(bytes, cdCursor + 42);
        std::string name = bytes.substr(cdCursor + 46, nameLen);
        cdCursor += 46 + nameLen;

        // Now read the local header + compressed data for this entry.
        assert(readU32(bytes, localOffset) == 0x04034b50u);
        std::uint16_t localNameLen = readU16(bytes, localOffset + 26);
        std::uint16_t localExtraLen = readU16(bytes, localOffset + 28);
        size_t dataOffset = localOffset + 30 + localNameLen + localExtraLen;
        std::string compressed = bytes.substr(dataOffset, compressedSize);

        std::string content = uncompressedSize == 0 ? std::string() : rawInflate(compressed, uncompressedSize);
        std::uint32_t actualCrc =
            static_cast<std::uint32_t>(crc32(0L, reinterpret_cast<const Bytef *>(content.data()),
                                              static_cast<uInt>(content.size())));
        assert(actualCrc == crc);  // content really does match the recorded checksum

        entries.push_back({name, content});
    }
    return entries;
}

}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_zip_archive_writer_test";
    fs::remove_all(root);
    fs::create_directories(root);

    // A round trip across a nested directory, a plain-text file, and a
    // file large/repetitive enough that deflate actually compresses it
    // (not just stores it near-verbatim) -- the case that would catch a
    // broken compressed-size/offset calculation most readily.
    {
        fs::path source = root / "source1";
        touch(source / "MANIFEST.txt", "Seabass anonymized library export\n");
        touch(source / "rekordbox" / "export.pdb", "fake pdb bytes, short");
        std::string repetitive(50000, 'a');
        touch(source / "engine" / "m.db", repetitive);

        fs::path zipPath = root / "export1.zip";
        writeZipArchive(source, zipPath);

        assert(fs::exists(zipPath));
        auto entries = parseZip(zipPath);
        assert(entries.size() == 3);

        bool foundManifest = false, foundPdb = false, foundDb = false;
        for (const auto &e : entries) {
            if (e.name == "MANIFEST.txt") {
                foundManifest = true;
                assert(e.content == "Seabass anonymized library export\n");
            } else if (e.name == "rekordbox/export.pdb") {
                foundPdb = true;
                assert(e.content == "fake pdb bytes, short");
            } else if (e.name == "engine/m.db") {
                foundDb = true;
                assert(e.content == repetitive);
            }
        }
        assert(foundManifest && foundPdb && foundDb);
        // The 50000-byte repetitive file should compress to well under
        // its original size -- a sanity check that this genuinely used
        // deflate, not just stored the bytes.
        assert(fs::file_size(zipPath) < repetitive.size());
        std::cout << "case 1 (nested directories, text + repetitive content round-trip correctly) OK\n";
    }

    // Archive-relative paths use forward slashes regardless of host OS
    // (checked directly above via the "rekordbox/export.pdb" name), and
    // are relative to sourceDir itself, not absolute -- reconfirmed here
    // with a deeper nesting level.
    {
        fs::path source = root / "source2";
        touch(source / "a" / "b" / "c" / "deep.txt", "deep content");
        fs::path zipPath = root / "export2.zip";
        writeZipArchive(source, zipPath);

        auto entries = parseZip(zipPath);
        assert(entries.size() == 1);
        assert(entries[0].name == "a/b/c/deep.txt");
        assert(entries[0].content == "deep content");
        std::cout << "case 2 (deeply nested path preserved as forward-slash archive name) OK\n";
    }

    // An empty source directory refuses rather than silently producing
    // a zero-entry (and likely unopenable-as-expected) zip file.
    {
        fs::path source = root / "empty_source";
        fs::create_directories(source);
        fs::path zipPath = root / "should_not_exist.zip";
        bool threw = false;
        try {
            writeZipArchive(source, zipPath);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        assert(threw);
        assert(!fs::exists(zipPath));
        std::cout << "case 3 (empty source directory: refuses, no partial zip left behind) OK\n";
    }

    // A source directory that doesn't exist at all refuses the same way.
    {
        fs::path source = root / "does_not_exist";
        fs::path zipPath = root / "also_should_not_exist.zip";
        bool threw = false;
        try {
            writeZipArchive(source, zipPath);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        assert(threw);
        std::cout << "case 4 (non-existent source directory: refuses) OK\n";
    }

    // The reader's side: a central-directory entry whose local-header
    // offset is damaged is refused, and nothing is written out of the
    // bytes it points at.
    //
    // Which check refuses it, measured rather than assumed: readU32()
    // bounds-checks before reading the local header's signature, so the
    // offset never reaches the arithmetic that finds the entry's data.
    // That arithmetic was a 32-bit sum of a uint32 offset and two uint16
    // lengths, which wraps for an offset near the top of the range; it
    // is widened now, and this case stays green with the narrow sum put
    // back, because the earlier read is what fires. Both halves are
    // worth having: the wrap is a trap for whoever reorders those three
    // lines, and this case is what says a damaged offset is refused at
    // all.
    {
        const fs::path source = root / "wrap-source";
        touch(source / "one.txt", std::string(64, 'a'));
        const fs::path zipPath = root / "wrap.zip";
        writeZipArchive(source, zipPath);

        std::string bytes;
        {
            std::ifstream in(zipPath, std::ios::binary);
            bytes.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
        // The central directory's local-header offset, last field of the
        // fixed part of the entry (at +42), set just under 2^32.
        const size_t eocd = bytes.rfind("PK\x05\x06");
        assert(eocd != std::string::npos);
        const size_t centralAt = static_cast<unsigned char>(bytes[eocd + 16])
            | (static_cast<unsigned char>(bytes[eocd + 17]) << 8)
            | (static_cast<unsigned char>(bytes[eocd + 18]) << 16)
            | (static_cast<unsigned char>(bytes[eocd + 19]) << 24);
        for (int i = 0; i < 4; ++i) {
            bytes[centralAt + 42 + i] = static_cast<char>(0xF0 + (i == 3 ? 0x0F : 0x0F));
        }
        bytes[centralAt + 42] = static_cast<char>(0xF0);
        bytes[centralAt + 43] = static_cast<char>(0xFF);
        bytes[centralAt + 44] = static_cast<char>(0xFF);
        bytes[centralAt + 45] = static_cast<char>(0xFF);  // 0xFFFFFFF0
        const fs::path damaged = root / "wrapped.zip";
        {
            std::ofstream out(damaged, std::ios::binary);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }

        const fs::path dest = root / "wrap-out";
        bool threw = false;
        try {
            seabass::infrastructure::extractZipArchive(damaged, dest);
        } catch (const std::runtime_error &) {
            threw = true;
        }
        if (!threw) {
            std::cerr << "a local-header offset of 0xFFFFFFF0 was accepted\n";
        }
        assert(threw && "an offset that wraps is not an offset inside the archive");
        assert(!fs::exists(dest / "one.txt") && "and nothing is written out of the bytes it landed on");
        std::cout << "case 5 (a damaged local-header offset is refused, nothing written) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
