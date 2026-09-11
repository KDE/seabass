// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "infrastructure/stick_backup/in_memory_archive_file.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"
#include "infrastructure/stick_backup/zip64_reader.hpp"
#include "infrastructure/stick_backup/zip64_writer.hpp"
#include "infrastructure/stick_backup/zip_format.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::stick_backup;
using seabass::infrastructure::hashing::Sha256;
using seabass::infrastructure::hashing::Sha256Digest;
namespace fs = std::filesystem;

namespace
{

std::string pseudoRandom(std::size_t size, std::uint64_t seed)
{
    std::string out(size, '\0');
    std::uint64_t x = seed * 0x9E3779B97F4A7C15ull + 1;
    for (std::size_t i = 0; i < size; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out[i] = static_cast<char>(x & 0xff);
    }
    return out;
}

struct Fixture
{
    std::string name;
    std::string content;
    std::int64_t mtime;
    bool directory = false;
};

// Writes every fixture (files streamed in 1 MiB chunks like the real
// backup does) plus a manifest, returns the boundaries.
WriterBoundaries writeFixtures(ArchiveFile &file, const std::vector<Fixture> &fixtures, BackupManifest &manifestOut)
{
    Zip64Writer writer(file, {});
    manifestOut = BackupManifest{};
    manifestOut.stickIdentifier = "uuid-1234";
    manifestOut.stickLabel = "TEST STICK";
    manifestOut.status = BackupStatus::Complete;
    manifestOut.createdAtUnix = 1'757'000'000;
    for (const Fixture &f : fixtures) {
        ManifestRow row;
        row.path = f.name;
        row.mtimeUnix = f.mtime;
        if (f.directory) {
            writer.addDirectory(f.name, f.mtime);
            row.kind = ManifestRow::Kind::Directory;
        } else {
            Zip64Writer::EntrySink sink = writer.beginFile(f.name, f.mtime);
            std::size_t pos = 0;
            while (pos < f.content.size()) {
                std::size_t take = std::min<std::size_t>(1u << 20, f.content.size() - pos);
                sink.write(zip::bytesOf(std::string_view(f.content).substr(pos, take)));
                pos += take;
            }
            CentralEntry entry = sink.finish();
            assert(entry.size == f.content.size());
            row.size = f.content.size();
            row.sha256 = sink.sha256();
            row.crc32 = entry.crc32;
            assert(row.sha256 == Sha256::of(f.content));
        }
        manifestOut.rows.push_back(row);
    }
    return writer.finish(manifestOut.serialize(), std::string(ManifestEntryName), manifestOut.createdAtUnix);
}

std::vector<Fixture> standardFixtures()
{
    std::vector<Fixture> fixtures;
    fixtures.push_back({"empty.bin", "", 1'600'000'000});
    fixtures.push_back({"one.bin", "x", 1'600'000'001});
    fixtures.push_back({"Contents", "", 1'600'000'002, true});
    fixtures.push_back({"Contents/Artist/Album/Disc 1/Sub/deeper/still/going/track.mp3", pseudoRandom(70'000, 3), 1'650'000'000});
    fixtures.push_back({"Contents/Caf\xC3\xA9.mp3", pseudoRandom(1'000, 4), 1'650'000'001});           // NFC
    fixtures.push_back({"Contents/Cafe\xCC\x81.mp3", pseudoRandom(1'001, 5), 1'650'000'002});          // NFD
    fixtures.push_back({"Engine Library/Database2/m.db", pseudoRandom(3 * (1u << 20) + 17, 6), 1'700'000'000});
    fixtures.push_back({"Engine Library/Music", "", 1'700'000'001, true});
    fixtures.push_back({"odd \\ back\tslash\nname.txt", "escaping", 1'700'000'002});
    fixtures.push_back({"ancient.txt", "before 1980", 100});  // DOS date can't hold it, extended timestamp can
    return fixtures;
}

void checkReaderAgainstFixtures(const Zip64Reader &reader, const std::vector<Fixture> &fixtures, const BackupManifest &manifest)
{
    assert(reader.entries().size() == fixtures.size() + 1);
    std::uint64_t footprintSum = 0;
    for (std::size_t i = 0; i < fixtures.size(); ++i) {
        const CentralEntry &entry = reader.entries()[i];
        const Fixture &f = fixtures[i];
        std::string expectedName = f.directory ? f.name + "/" : f.name;
        assert(entry.name == expectedName);
        assert(entry.isDirectory == f.directory);
        assert(entry.mtimeUnix == f.mtime);
        assert(entry.size == f.content.size());
        assert(reader.findEntry(expectedName) == i);
        if (!f.directory) {
            assert(reader.readEntryToString(i) == f.content);
            assert(reader.verifyCrc(i));
            assert(entry.hasDataDescriptor);
        }
        footprintSum += reader.footprint(i);
        const ManifestRow *row = manifest.findRow(f.name);
        assert(row != nullptr);
        assert(row->size == f.content.size());
        if (!f.directory) {
            assert(row->sha256 == Sha256::of(f.content));
        }
    }
    std::size_t manifestIndex = fixtures.size();
    assert(reader.entries()[manifestIndex].name == ManifestEntryName);
    footprintSum += reader.footprint(manifestIndex);
    // A freshly written archive has no dead space: entries tile the file
    // exactly up to the central directory.
    assert(footprintSum == reader.layout().centralDirectoryOffset);
    assert(reader.layout().entryCount == fixtures.size() + 1);
}

}  // namespace

int main()
{
    // ---- Round trip through memory ----
    {
        auto file = std::make_unique<InMemoryArchiveFile>();
        std::vector<Fixture> fixtures = standardFixtures();
        BackupManifest manifest;
        WriterBoundaries b = writeFixtures(*file, fixtures, manifest);
        assert(b.endOffset == file->size());
        assert(b.manifestOffset < b.centralDirectoryOffset);
        assert(b.centralDirectoryOffset < b.zip64EndOfCentralDirectoryOffset);
        assert(b.zip64EndOfCentralDirectoryOffset + zip::Zip64EndOfCentralDirectorySize == b.zip64LocatorOffset);
        assert(b.zip64LocatorOffset + zip::Zip64LocatorSize == b.endOfCentralDirectoryOffset);
        assert(b.endOfCentralDirectoryOffset + zip::EndOfCentralDirectorySize == b.endOffset);

        Zip64Reader reader = Zip64Reader::open(*file);
        assert(reader.layout().centralDirectoryOffset == b.centralDirectoryOffset);
        assert(reader.layout().zip64EndOfCentralDirectoryOffset == b.zip64EndOfCentralDirectoryOffset);
        assert(reader.layout().endOfCentralDirectoryOffset == b.endOfCentralDirectoryOffset);

        std::string manifestText = reader.readEntryToString(*reader.findEntry(ManifestEntryName));
        std::string error;
        auto parsed = BackupManifest::parse(manifestText, &error);
        assert(parsed.has_value());
        assert(parsed->stickIdentifier == "uuid-1234" && parsed->stickLabel == "TEST STICK");
        assert(parsed->status == BackupStatus::Complete);
        assert(parsed->rows.size() == fixtures.size());
        checkReaderAgainstFixtures(reader, fixtures, *parsed);
        std::cout << "case 1 (in-memory round trip: entries, names incl. NFC/NFD/escapes, sizes, mtimes, CRCs, manifest, no dead space) OK\n";
    }

    // ---- Round trip through a real file ----
    {
        fs::path root = seabass::testing::scratchRoot() / "seabass_backup_archive_roundtrip_test";
        fs::remove_all(root);
        fs::create_directories(root);
        fs::path path = root / "stick.zip";
        std::vector<Fixture> fixtures = standardFixtures();
        BackupManifest manifest;
        {
            PosixArchiveFile file(path, PosixArchiveFile::OpenMode::ReadWrite);
            writeFixtures(file, fixtures, manifest);
            file.barrier();
        }
        {
            PosixArchiveFile file(path, PosixArchiveFile::OpenMode::ReadOnly);
            Zip64Reader reader = Zip64Reader::open(file);
            checkReaderAgainstFixtures(reader, fixtures, manifest);
        }
        fs::remove_all(root);
        std::cout << "case 2 (on-disk round trip) OK\n";
    }

    // ---- Corruption is never silent ----
    {
        std::vector<Fixture> fixtures = standardFixtures();
        BackupManifest manifest;
        InMemoryArchiveFile pristine;
        WriterBoundaries b = writeFixtures(pristine, fixtures, manifest);

        // A flipped signature byte inside the central directory: open() refuses.
        {
            std::vector<std::byte> bytes = pristine.bytes();
            bytes[static_cast<std::size_t>(b.centralDirectoryOffset)] ^= std::byte{0x01};
            InMemoryArchiveFile broken(bytes);
            std::string error;
            assert(!Zip64Reader::tryOpen(broken, &error).has_value());
            assert(!error.empty());
        }
        // A flipped byte in an entry's *name* in the central directory
        // parses fine structurally -- ZIP has no checksum there. Two
        // independent nets catch it: the manifest no longer lists the
        // name, and the local header's own copy of the name disagrees the
        // moment the entry is touched.
        {
            std::vector<std::byte> bytes = pristine.bytes();
            std::size_t nameByte = static_cast<std::size_t>(b.centralDirectoryOffset) + zip::CentralDirectoryEntrySize + 2;
            bytes[nameByte] ^= std::byte{0x20};  // 'p' <-> 'P' in "empty.bin"
            InMemoryArchiveFile renamed(bytes);
            Zip64Reader reader = Zip64Reader::open(renamed);
            std::string manifestText = reader.readEntryToString(*reader.findEntry(ManifestEntryName));
            auto parsed = BackupManifest::parse(manifestText);
            assert(parsed.has_value());
            assert(parsed->findRow(reader.entries()[0].name) == nullptr);
            bool threw = false;
            try {
                reader.verifyCrc(0);
            } catch (const ArchiveFormatError &e) {
                threw = std::string(e.what()).find("local header name differs") != std::string::npos;
            }
            assert(threw);
        }
        // A flipped data byte: CRC catches it.
        {
            std::vector<std::byte> bytes = pristine.bytes();
            Zip64Reader clean = Zip64Reader::open(pristine);
            std::size_t victim = *clean.findEntry("Engine Library/Database2/m.db");
            bytes[static_cast<std::size_t>(clean.dataOffset(victim)) + 12345] ^= std::byte{0x80};
            InMemoryArchiveFile damaged(bytes);
            Zip64Reader reader = Zip64Reader::open(damaged);
            assert(!reader.verifyCrc(victim));
            assert(reader.verifyCrc(victim - 1));
        }
        // A tampered manifest fails its own hash.
        {
            std::string text = manifest.serialize();
            std::size_t pos = text.find("TEST STICK");
            text[pos] = 'B';
            std::string error;
            assert(!BackupManifest::parse(text, &error).has_value());
            assert(error == "manifest hash mismatch");
            assert(!BackupManifest::parse("", &error).has_value());
            assert(!BackupManifest::parse("garbage\n", &error).has_value());
        }
        // Truncated anywhere inside the trailer: refused, never misread.
        for (std::uint64_t cut = b.centralDirectoryOffset; cut < b.endOffset; cut += 7) {
            InMemoryArchiveFile truncated(pristine.truncatedImage(cut));
            assert(!Zip64Reader::tryOpen(truncated).has_value());
        }
        std::cout << "case 3 (CD corruption refused, name flip caught by manifest, data flip caught by CRC, manifest tamper refused, trailer truncations refused) OK\n";
    }

    // ---- Writer discipline ----
    {
        InMemoryArchiveFile file;
        Zip64Writer writer(file, {});
        {
            Zip64Writer::EntrySink open = writer.beginFile("a.bin", 1);
            open.write(zip::bytesOf("partial data that will be abandoned"));
            bool threw = false;
            try {
                writer.beginFile("b.bin", 2);
            } catch (const std::logic_error &) {
                threw = true;
            }
            assert(threw);
            // `open` goes out of scope unfinished: abandoned entry.
        }
        std::uint64_t abandonedBytes = file.size();
        assert(writer.entries().empty());
        writer.addFileFromMemory("b.bin", 2, zip::bytesOf("kept"));
        BackupManifest manifest;
        manifest.createdAtUnix = 1;
        WriterBoundaries b = writer.finish(manifest.serialize(), std::string(ManifestEntryName), 1);
        Zip64Reader reader = Zip64Reader::open(file);
        assert(reader.entries().size() == 2);
        assert(reader.entries()[0].name == "b.bin");
        std::uint64_t footprints = reader.footprint(0) + reader.footprint(1);
        // The abandoned bytes are exactly the dead space.
        assert(footprints + abandonedBytes == b.centralDirectoryOffset);
        std::cout << "case 4 (one sink at a time; an abandoned sink is unlisted dead space) OK\n";
    }

    // ---- ZIP64 entry counts (> 65 535) ----
    {
        InMemoryArchiveFile file;
        Zip64Writer writer(file, {});
        const std::size_t count = 70'000;
        for (std::size_t i = 0; i < count; ++i) {
            writer.addFileFromMemory("f/" + std::to_string(i), static_cast<std::int64_t>(i), zip::bytesOf(""));
        }
        BackupManifest manifest;
        manifest.createdAtUnix = 1;
        WriterBoundaries b = writer.finish(manifest.serialize(), std::string(ManifestEntryName), 1);
        // The 16-bit EOCD count saturates; the zip64 record carries the truth.
        std::vector<std::byte> eocd(zip::EndOfCentralDirectorySize);
        file.readAt(b.endOfCentralDirectoryOffset, eocd);
        assert(zip::readU16(eocd, 10) == zip::Max16);
        Zip64Reader reader = Zip64Reader::open(file);
        assert(reader.entries().size() == count + 1);
        assert(reader.entries()[69'999].name == "f/69999");
        assert(reader.findEntry("f/12345") == 12345);
        std::cout << "case 5 (70 000 entries: zip64 entry count) OK\n";
    }

    // ---- > 4 GiB entry (opt-in: writes ~4 GiB to the temp directory) ----
    if (const char *large = std::getenv("SEABASS_LARGE_TESTS"); large != nullptr && std::string(large) == "1") {
        fs::path root = seabass::testing::scratchRoot() / "seabass_backup_archive_roundtrip_large";
        fs::remove_all(root);
        fs::create_directories(root);
        fs::path path = root / "large.zip";
        const std::uint64_t bigSize = (std::uint64_t{1} << 32) + 5;
        {
            PosixArchiveFile file(path, PosixArchiveFile::OpenMode::ReadWrite);
            Zip64Writer writer(file, {});
            Zip64Writer::EntrySink sink = writer.beginFile("big.bin", 1);
            std::string chunk(64u << 20, '\0');
            std::uint64_t remaining = bigSize;
            while (remaining > 0) {
                std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, chunk.size()));
                sink.write(zip::bytesOf(std::string_view(chunk).substr(0, take)));
                remaining -= take;
            }
            CentralEntry big = sink.finish();
            assert(big.size == bigSize);
            writer.addFileFromMemory("after.bin", 2, zip::bytesOf("after the 4 GiB mark"));
            BackupManifest manifest;
            manifest.createdAtUnix = 1;
            writer.finish(manifest.serialize(), std::string(ManifestEntryName), 1);
        }
        {
            PosixArchiveFile file(path, PosixArchiveFile::OpenMode::ReadOnly);
            Zip64Reader reader = Zip64Reader::open(file);
            assert(reader.entries()[0].size == bigSize);
            assert(reader.entries()[1].localHeaderOffset > (std::uint64_t{1} << 32));
            assert(reader.readEntryToString(1) == "after the 4 GiB mark");
            assert(reader.verifyCrc(1));
        }
        fs::remove_all(root);
        std::cout << "case 6 (> 4 GiB entry and > 4 GiB offsets via zip64 extra fields) OK\n";
    } else {
        std::cout << "case 6 (> 4 GiB entry) skipped -- set SEABASS_LARGE_TESTS=1 to run\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
