// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Deflate in the backup archive.
//
// The backup of a save is one archive rather than hundreds of loose files,
// and it stays on the stick permanently -- so its size is a cost that never
// goes away. Analysis files compress to about 70%, which is worth roughly
// 26 MB on a 400-file save (docs/write-path-performance.md, rounds 15-16).
//
// What has to hold:
//   - a deflated entry reads back byte-identical;
//   - the CRC and the SHA-256 describe the ORIGINAL bytes, not the stored
//     ones, or the manifest and every existing integrity check would be
//     comparing against the wrong thing;
//   - compression is per entry, because audio does not compress and paying
//     deflate for an MP3 is CPU for nothing;
//   - and a STORE-only archive written by the previous version still reads,
//     because every backup already on a real stick is in that format.

#include <cassert>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include <zlib.h>

#include "infrastructure/stick_backup/in_memory_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"
#include "infrastructure/stick_backup/zip64_writer.hpp"
#include "infrastructure/stick_backup/zip_format.hpp"

using namespace seabass::infrastructure::stick_backup;
using seabass::infrastructure::hashing::Sha256Digest;

namespace
{

std::span<const std::byte> bytes(const std::string &s)
{
    return std::as_bytes(std::span<const char>(s.data(), s.size()));
}

// Text-shaped, so it actually compresses -- random bytes would deflate to
// slightly MORE than their input and prove nothing about the win.
std::string compressible(std::size_t size)
{
    static const std::string unit = "cue point at 0:00, memory, track ";
    std::string out;
    out.reserve(size + unit.size());
    while (out.size() < size) {
        out += unit + std::to_string(out.size());
    }
    out.resize(size);
    return out;
}

std::string incompressible(std::size_t size)
{
    std::string out(size, '\0');
    std::uint64_t x = 0x243F6A8885A308D3ull;
    for (std::size_t i = 0; i < size; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out[i] = static_cast<char>(x & 0xff);
    }
    return out;
}

std::uint32_t crcOf(const std::string &s)
{
    return static_cast<std::uint32_t>(
        ::crc32(0L, reinterpret_cast<const Bytef *>(s.data()), static_cast<uInt>(s.size())));
}

}  // namespace

int main()
{
    const std::string payload = compressible(400 * 1024);
    const std::string noise = incompressible(64 * 1024);

    // ---- case 1: a deflated entry round-trips, and is smaller ----------
    {
        InMemoryArchiveFile file;
        Zip64Writer writer(file, {});
        Sha256Digest storedDigest{};
        Sha256Digest deflatedDigest{};
        CentralEntry stored =
            writer.addFileFromMemory("stored.EXT", 1700000000, bytes(payload), &storedDigest,
                                     Compression::Store);
        CentralEntry deflated =
            writer.addFileFromMemory("deflated.EXT", 1700000000, bytes(payload), &deflatedDigest,
                                     Compression::Deflate);
        writer.finish("{}", "manifest.json", 1700000000);

        assert(stored.method == zip::MethodStore);
        assert(deflated.method == zip::MethodDeflate);
        assert(stored.compressedSize == stored.size);
        assert(deflated.size == payload.size());
        assert(deflated.compressedSize < deflated.size);

        // The digest describes the content, so it cannot depend on how the
        // content was stored.
        assert(storedDigest == deflatedDigest);
        // Same for the CRC, which the ZIP format defines over the
        // uncompressed bytes.
        assert(deflated.crc32 == crcOf(payload));
        assert(stored.crc32 == deflated.crc32);

        Zip64Reader reader = Zip64Reader::open(file);
        const auto deflatedIndex = reader.findEntry("deflated.EXT");
        assert(deflatedIndex.has_value());
        assert(reader.readEntryToString(*deflatedIndex) == payload);
        assert(reader.verifyCrc(*deflatedIndex));
        // Third-party tools read the descriptor, so it has to be right too.
        assert(reader.verifyDataDescriptor(*deflatedIndex));

        const auto storedIndex = reader.findEntry("stored.EXT");
        assert(reader.readEntryToString(*storedIndex) == payload);

        std::cout << "case 1: " << payload.size() << " -> " << deflated.compressedSize << " bytes ("
                  << (100 * deflated.compressedSize / deflated.size) << "%)\n";
    }

    // ---- case 2: per-entry choice, and incompressible data is honest ---
    {
        InMemoryArchiveFile file;
        Zip64Writer writer(file, {});
        CentralEntry audio =
            writer.addFileFromMemory("track.mp3", 1700000000, bytes(noise), nullptr, Compression::Store);
        CentralEntry cues =
            writer.addFileFromMemory("ANLZ0000.EXT", 1700000000, bytes(payload), nullptr, Compression::Deflate);
        writer.finish("{}", "manifest.json", 1700000000);

        assert(audio.method == zip::MethodStore);
        assert(cues.method == zip::MethodDeflate);

        Zip64Reader reader = Zip64Reader::open(file);
        assert(reader.readEntryToString(*reader.findEntry("track.mp3")) == noise);
        assert(reader.readEntryToString(*reader.findEntry("ANLZ0000.EXT")) == payload);
    }

    // ---- case 3: deflating noise must still round-trip ------------------
    // It will not get smaller, and may get slightly larger; correctness is
    // the point, not the ratio.
    {
        InMemoryArchiveFile file;
        Zip64Writer writer(file, {});
        writer.addFileFromMemory("noise.bin", 1700000000, bytes(noise), nullptr, Compression::Deflate);
        writer.finish("{}", "manifest.json", 1700000000);

        Zip64Reader reader = Zip64Reader::open(file);
        assert(reader.readEntryToString(*reader.findEntry("noise.bin")) == noise);
        assert(reader.verifyCrc(*reader.findEntry("noise.bin")));
    }

    // ---- case 4: a STORE-only archive still reads -----------------------
    // Every backup already sitting on a real stick was written before this
    // existed. Nothing here may make those unreadable.
    {
        InMemoryArchiveFile file;
        Zip64Writer writer(file, {});
        writer.addDirectory("PIONEER/", 1700000000);
        writer.addFileFromMemory("PIONEER/export.pdb", 1700000000, bytes(payload));  // defaults to Store
        writer.finish("{}", "manifest.json", 1700000000);

        Zip64Reader reader = Zip64Reader::open(file);
        for (const CentralEntry &entry : reader.entries()) {
            assert(entry.method == zip::MethodStore);
            assert(entry.compressedSize == entry.size);
        }
        assert(reader.readEntryToString(*reader.findEntry("PIONEER/export.pdb")) == payload);
    }

    // ---- case 5: streamed in pieces, which is how a real save writes ----
    {
        InMemoryArchiveFile file;
        Zip64Writer writer(file, {});
        {
            Zip64Writer::EntrySink sink =
                writer.beginFile("streamed.EXT", 1700000000, Compression::Deflate);
            for (std::size_t at = 0; at < payload.size(); at += 7919) {  // deliberately awkward
                const std::size_t take = std::min<std::size_t>(7919, payload.size() - at);
                sink.write(bytes(payload).subspan(at, take));
            }
            CentralEntry entry = sink.finish();
            assert(entry.size == payload.size());
            assert(entry.compressedSize == sink.compressedBytesWritten());
            assert(entry.crc32 == crcOf(payload));
        }
        writer.finish("{}", "manifest.json", 1700000000);

        Zip64Reader reader = Zip64Reader::open(file);
        assert(reader.readEntryToString(*reader.findEntry("streamed.EXT")) == payload);
        assert(reader.verifyCrc(*reader.findEntry("streamed.EXT")));
        assert(reader.verifyDataDescriptor(*reader.findEntry("streamed.EXT")));
    }

    // ---- case 6: an empty deflated entry --------------------------------
    // Zero-length files exist and the deflate stream still has a terminator,
    // so compressedSize is not zero. Easy to get wrong.
    {
        InMemoryArchiveFile file;
        Zip64Writer writer(file, {});
        CentralEntry empty =
            writer.addFileFromMemory("empty.EXT", 1700000000, {}, nullptr, Compression::Deflate);
        writer.finish("{}", "manifest.json", 1700000000);

        assert(empty.size == 0);
        assert(empty.crc32 == 0);

        Zip64Reader reader = Zip64Reader::open(file);
        assert(reader.readEntryToString(*reader.findEntry("empty.EXT")).empty());
        assert(reader.verifyDataDescriptor(*reader.findEntry("empty.EXT")));
    }

    std::cout << "backup_archive_deflate_test: all cases passed\n";
    return 0;
}
