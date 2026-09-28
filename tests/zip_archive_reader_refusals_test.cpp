// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What extractZipArchive() refuses, one malformation at a time.
//
// Every archive here starts as a valid one written by the project's own
// writer and is then damaged byte by byte, so each case differs from a
// good archive in exactly one field. Each refusal is asserted by its own
// message, so a case that is refused by some other, earlier check fails
// here rather than passing for the wrong reason, and each asserts that
// nothing was written out of the damaged entry.

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/zip_archive_reader.hpp"
#include "infrastructure/zip_archive_writer.hpp"

#include "scratch_path.hpp"

using seabass::infrastructure::extractZipArchive;
using seabass::infrastructure::writeZipArchive;
namespace fs = std::filesystem;

namespace
{

fs::path g_root;

void writeBytes(const fs::path &path, const std::string &bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string readBytes(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::uint32_t u32At(const std::string &bytes, size_t at)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at]))
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 16)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3])) << 24);
}

std::uint16_t u16At(const std::string &bytes, size_t at)
{
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[at])
                                      | (static_cast<unsigned char>(bytes[at + 1]) << 8));
}

void putU32(std::string &bytes, size_t at, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        bytes[at + static_cast<size_t>(i)] = static_cast<char>((value >> (8 * i)) & 0xff);
    }
}

void putU16(std::string &bytes, size_t at, std::uint16_t value)
{
    bytes[at] = static_cast<char>(value & 0xff);
    bytes[at + 1] = static_cast<char>((value >> 8) & 0xff);
}

// A one-entry archive from the project's writer, and where its three
// records sit. The writer adds no archive comment, so the end-of-central-
// directory record is the last 22 bytes; its offset field (+16) names the
// central directory entry, whose field at +42 names the local header.
struct Archive
{
    std::string bytes;
    size_t eocd = 0;
    size_t central = 0;
    size_t local = 0;

    // Where the entry's data starts: after the 30-byte local header, its
    // name and its extra field.
    size_t dataAt() const { return local + 30 + u16At(bytes, local + 26) + u16At(bytes, local + 28); }
    std::uint32_t compressedSize() const { return u32At(bytes, central + 20); }
    std::uint16_t method() const { return u16At(bytes, central + 10); }
};

Archive oneEntryArchive(const std::string &caseName, const std::string &entryName, const std::string &content)
{
    const fs::path source = g_root / (caseName + "-source");
    writeBytes(source / seabass::pathFromUtf8(entryName), content);
    const fs::path zip = g_root / (caseName + "-good.zip");
    writeZipArchive(source, zip);
    Archive archive;
    archive.bytes = readBytes(zip);
    archive.eocd = archive.bytes.size() - 22;
    assert(u32At(archive.bytes, archive.eocd) == 0x06054b50u && "the precondition: no archive comment");
    assert(u16At(archive.bytes, archive.eocd + 10) == 1 && "the precondition: one entry");
    archive.central = u32At(archive.bytes, archive.eocd + 16);
    assert(u32At(archive.bytes, archive.central) == 0x02014b50u);
    archive.local = u32At(archive.bytes, archive.central + 42);
    assert(u32At(archive.bytes, archive.local) == 0x04034b50u);
    assert(archive.bytes.substr(archive.central + 46, u16At(archive.bytes, archive.central + 28)) == entryName);
    return archive;
}

// The name, patched in both the local header and the central directory,
// the same length as before so no offset moves.
void renameEntry(Archive &archive, const std::string &from, const std::string &to)
{
    assert(from.size() == to.size());
    assert(archive.bytes.compare(archive.local + 30, from.size(), from) == 0);
    assert(archive.bytes.compare(archive.central + 46, from.size(), from) == 0);
    archive.bytes.replace(archive.local + 30, to.size(), to);
    archive.bytes.replace(archive.central + 46, to.size(), to);
}

// Extracts `bytes` into a fresh destination and returns the refusal, or
// an empty string when the archive was accepted.
std::string refusalFor(const std::string &caseName, const std::string &bytes, fs::path *destOut = nullptr)
{
    const fs::path zip = g_root / (caseName + ".zip");
    writeBytes(zip, bytes);
    const fs::path dest = g_root / (caseName + "-out");
    fs::remove_all(dest);
    if (destOut != nullptr) {
        *destOut = dest;
    }
    try {
        extractZipArchive(zip, dest);
    } catch (const std::runtime_error &e) {
        return e.what();
    }
    return {};
}

bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Every regular file under `dir`: what a refused extraction left behind.
size_t filesUnder(const fs::path &dir)
{
    size_t count = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_regular_file()) {
            ++count;
        }
    }
    return count;
}

void expectRefusal(const std::string &caseName, const std::string &bytes, const std::string &expected)
{
    fs::path dest;
    const std::string refusal = refusalFor(caseName, bytes, &dest);
    if (!contains(refusal, expected)) {
        std::cerr << caseName << ": expected a refusal containing \"" << expected << "\", got \"" << refusal << "\"\n";
    }
    assert(contains(refusal, expected));
    assert(filesUnder(dest) == 0 && "a refused archive writes nothing");
}

}  // namespace

int main()
{
    g_root = seabass::testing::scratchRoot() / "seabass_zip_archive_reader_refusals_test";
    fs::remove_all(g_root);
    fs::create_directories(g_root);

    // Compressible, so the writer deflates it and the inflate path is the
    // one a good archive takes.
    const std::string content(4096, 'x');

    // The baseline every damaged copy below is measured against: the
    // untouched archive extracts, into the nested path it names.
    {
        Archive good = oneEntryArchive("good", "PIONEER/rekordbox/export.pdb", content);
        assert(good.method() == 8 && "the precondition: the writer deflated the entry");
        fs::path dest;
        assert(refusalFor("good-copy", good.bytes, &dest).empty());
        assert(readBytes(dest / "PIONEER" / "rekordbox" / "export.pdb") == content);
        std::cout << "case 1 (the undamaged archive extracts into its nested path) OK\n";
    }

    // Not there at all.
    {
        bool threw = false;
        try {
            extractZipArchive(g_root / "no-such.zip", g_root / "no-such-out");
        } catch (const std::runtime_error &e) {
            threw = contains(e.what(), "zip: could not open");
        }
        assert(threw);
        assert(!fs::exists(g_root / "no-such-out") && "no destination is made for an archive that is not there");
        std::cout << "case 2 (a missing archive: could not open) OK\n";
    }

    Archive base = oneEntryArchive("base", "one.txt", content);

    // Shorter than the smallest possible end-of-central-directory record.
    expectRefusal("too-small", base.bytes.substr(0, 21), "zip: too small to be an archive");
    std::cout << "case 3 (21 bytes: too small to be an archive) OK\n";

    // Truncated inside the central directory: the end record is gone
    // with it, and nothing else in the file looks like one.
    expectRefusal("truncated", base.bytes.substr(0, base.bytes.size() - 10),
                  "zip: no end-of-central-directory record");
    std::cout << "case 4 (truncated: no end-of-central-directory record) OK\n";

    // The end record's signature zeroed.
    {
        std::string bytes = base.bytes;
        putU32(bytes, base.eocd, 0);
        expectRefusal("no-eocd", bytes, "zip: no end-of-central-directory record");
        std::cout << "case 5 (end-of-central-directory signature zeroed) OK\n";
    }

    // The end record names a central directory past the end of the file.
    {
        std::string bytes = base.bytes;
        putU32(bytes, base.eocd + 16, static_cast<std::uint32_t>(bytes.size() + 100));
        expectRefusal("cd-past-end", bytes, "zip: truncated");
        std::cout << "case 6 (central directory offset past the end: truncated) OK\n";
    }

    // A central directory entry whose signature is wrong.
    {
        std::string bytes = base.bytes;
        putU32(bytes, base.central, 0x12345678u);
        expectRefusal("bad-central", bytes, "zip: central directory entry 0 is malformed");
        std::cout << "case 7 (central entry signature corrupted) OK\n";
    }

    // A central directory entry that claims a longer name than the
    // archive has bytes left: refused, not read as a shorter name.
    {
        std::string bytes = base.bytes;
        putU16(bytes, base.central + 28, 0xFFFF);
        expectRefusal("long-name", bytes, "names more bytes than the archive holds");
        std::cout << "case 8 (central entry name length past the end) OK\n";
    }

    // A local header whose signature is wrong.
    {
        std::string bytes = base.bytes;
        putU32(bytes, base.local, 0x87654321u);
        expectRefusal("bad-local", bytes, "zip: local header for \"one.txt\" is malformed");
        std::cout << "case 9 (local header signature corrupted) OK\n";
    }

    // A compressed size that runs past the end of the archive.
    {
        std::string bytes = base.bytes;
        putU32(bytes, base.central + 20, static_cast<std::uint32_t>(bytes.size()));
        expectRefusal("data-past-end", bytes, "data for \"one.txt\" runs past the end of the archive");
        std::cout << "case 10 (compressed size past the end) OK\n";
    }

    // A compression method this reader does not know.
    {
        std::string bytes = base.bytes;
        putU16(bytes, base.central + 10, 99);
        expectRefusal("method-99", bytes, "\"one.txt\" uses an unsupported compression method");
        std::cout << "case 11 (unsupported compression method) OK\n";
    }

    // Deflated data replaced with garbage of the same length. 0xFF starts
    // a block of the reserved type 3, which no inflater accepts.
    {
        std::string bytes = base.bytes;
        for (size_t i = 0; i < base.compressedSize(); ++i) {
            bytes[base.dataAt() + i] = static_cast<char>(0xFF);
        }
        expectRefusal("garbage-deflate", bytes, "zip: decompression failed");
        std::cout << "case 12 (garbage in place of deflated data: decompression failed) OK\n";
    }

    // A stored entry (method 0) is copied through as it is: the method
    // switched to stored, so what lands on disk is the deflated bytes.
    {
        std::string bytes = base.bytes;
        putU16(bytes, base.central + 10, 0);
        fs::path dest;
        assert(refusalFor("stored", bytes, &dest).empty());
        assert(readBytes(dest / "one.txt") == base.bytes.substr(base.dataAt(), base.compressedSize()));
        std::cout << "case 13 (a stored entry is copied through byte for byte) OK\n";
    }

    // The entry's name is a file already there as a directory: the write
    // is refused by name.
    {
        const fs::path zip = g_root / "blocked.zip";
        writeBytes(zip, base.bytes);
        const fs::path dest = g_root / "blocked-out";
        fs::create_directories(dest / "one.txt");
        std::string refusal;
        try {
            extractZipArchive(zip, dest);
        } catch (const std::runtime_error &e) {
            refusal = e.what();
        }
        assert(contains(refusal, "zip: could not write ") && contains(refusal, "one.txt"));
        assert(fs::is_directory(dest / "one.txt"));
        std::cout << "case 14 (a target that cannot be written is refused by name) OK\n";
    }

    // The path-traversal guard. Each name is patched into both headers at
    // the same length, so the archive is otherwise the one case 1 showed
    // extracting.
    {
        Archive archive = oneEntryArchive("traversal-up", "aa/x", content);
        renameEntry(archive, "aa/x", "../x");
        expectRefusal("traversal-up", archive.bytes, "escapes the destination: \"../x\"");
        assert(!fs::exists(g_root / "x") && "nothing was written beside the destination");
        std::cout << "case 15 (\"../x\" is refused) OK\n";
    }
    {
        Archive archive = oneEntryArchive("traversal-mid", "a/bb/x", content);
        renameEntry(archive, "a/bb/x", "a/../x");
        expectRefusal("traversal-mid", archive.bytes, "escapes the destination: \"a/../x\"");
        std::cout << "case 16 (\"a/../x\", a .. in the middle, is refused) OK\n";
    }
    {
        Archive archive = oneEntryArchive("traversal-abs", "tmp/x", content);
        renameEntry(archive, "tmp/x", "/tm/x");
        expectRefusal("traversal-abs", archive.bytes, "refusing absolute entry name \"/tm/x\"");
        std::cout << "case 17 (\"/tm/x\", an absolute name, is refused) OK\n";
    }
    {
        Archive archive = oneEntryArchive("traversal-drive", "Cx/x", content);
        renameEntry(archive, "Cx/x", "C:/x");
        expectRefusal("traversal-drive", archive.bytes, "refusing absolute entry name \"C:/x\"");
        std::cout << "case 18 (\"C:/x\", a drive-letter name, is refused) OK\n";
    }
    {
        // An empty name: the central entry's name length zeroed. The
        // entry still points at a good local header, so this is the name
        // check refusing it and nothing earlier.
        std::string bytes = base.bytes;
        putU16(bytes, base.central + 28, 0);
        expectRefusal("empty-name", bytes, "refusing absolute entry name \"\"");
        std::cout << "case 19 (an empty entry name is refused) OK\n";
    }
    {
        // "./" parts are skipped rather than refused: the file lands at
        // the plain nested path.
        Archive archive = oneEntryArchive("dot-part", "a/bb/x", content);
        renameEntry(archive, "a/bb/x", "./a/xx");
        fs::path dest;
        assert(refusalFor("dot-part", archive.bytes, &dest).empty());
        assert(readBytes(dest / "a" / "xx") == content);
        std::cout << "case 20 (a \".\" part is skipped, the nested path is accepted) OK\n";
    }

    fs::remove_all(g_root);
    std::cout << "all cases passed\n";
    return 0;
}
