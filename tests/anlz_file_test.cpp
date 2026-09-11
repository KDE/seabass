// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "infrastructure/rekordbox/anlz_file.hpp"
#include "infrastructure/rekordbox/big_endian.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::rekordbox;
namespace fs = std::filesystem;

namespace
{

// A minimal, but structurally real, ANLZ file: a 12-byte header (just
// magic/len_header/len_file, no extra padding) and one section with a
// 12-byte (header-only, no body) tag. Good enough to exercise the
// section-framing walk without needing real hot-cue/waveform data.
std::string buildMinimalAnlz()
{
    std::string data = "PMAI";
    appendU32BE(data, 12);  // len_header
    appendU32BE(data, 0);   // len_file -- overwritten by writeRaw(), doesn't matter here

    std::string section = "TEST";
    appendU32BE(section, 8);   // section's own inner len_header (unused/uninterpreted by AnlzFile)
    appendU32BE(section, 12);  // len_tag: header-only, no body
    data += section;

    writeU32BE(data, 8, static_cast<uint32_t>(data.size()));  // len_file, now that we know the real size
    return data;
}

void writeFile(const fs::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_anlz_file_test";
    fs::remove_all(root);
    fs::create_directories(root);
    fs::path path = root / "ANLZ0000.EXT";

    std::string minimal = buildMinimalAnlz();

    // Read/write round trip: reading a valid file back and writing it
    // out unmodified reproduces it byte-for-byte.
    {
        writeFile(path, minimal);
        auto file = AnlzFile::readRaw(path.string());
        assert(file.sections.size() == 1);
        assert(file.sections[0].fourcc == 0x54455354);  // "TEST"

        file.writeRaw(path.string());
        assert(readFile(path) == minimal);
        std::cout << "case 1 (read/write round trip, unmodified -> byte-identical) OK\n";
    }

    // A genuine edit (replacing the one section's bytes) is reflected
    // correctly, and len_file is recomputed to match the new size.
    {
        writeFile(path, minimal);
        auto file = AnlzFile::readRaw(path.string());

        std::string newSection = "TEST";
        appendU32BE(newSection, 8);
        appendU32BE(newSection, 16);  // now has a 4-byte body
        newSection += "abcd";
        file.sections[0] = {0x54455354, newSection};

        file.writeRaw(path.string());
        auto reread = AnlzFile::readRaw(path.string());
        assert(reread.sections[0].rawBytes == newSection);
        assert(readU32BE(readFile(path), 8) == readFile(path).size());  // len_file matches actual size
        std::cout << "case 2 (real edit -> len_file recomputed, re-reads correctly) OK\n";
    }

    // Bad magic is rejected at read time.
    {
        writeFile(path, "NOPE" + minimal.substr(4));
        bool threw = false;
        try {
            AnlzFile::readRaw(path.string());
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);
        std::cout << "case 3 (bad magic rejected at read time) OK\n";
    }

    // Staleness check: if the file changes on disk between readRaw()
    // and writeRaw() (back to the same path), writeRaw() must refuse
    // rather than clobber that change with a stale copy.
    {
        writeFile(path, minimal);
        auto file = AnlzFile::readRaw(path.string());

        std::string externallyModified = minimal + std::string(4, '\0');  // different size is enough
        writeFile(path, externallyModified);

        bool threw = false;
        try {
            file.writeRaw(path.string());
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);
        assert(readFile(path) == externallyModified);  // never touched
        std::cout << "case 4 (file changed since read -> writeRaw() refuses) OK\n";
    }

    // Pre-write self-check: corrupting a section's declared length (the
    // public `sections` field can be mutated directly by any caller) so
    // the reassembled bytes would no longer be a valid ANLZ file must be
    // caught before anything is written to disk.
    {
        writeFile(path, minimal);
        auto file = AnlzFile::readRaw(path.string());

        std::string corrupted = "TEST";
        appendU32BE(corrupted, 8);
        appendU32BE(corrupted, 3);  // len_tag below the 12-byte minimum -- invalid
        file.sections[0] = {0x54455354, corrupted};

        std::string before = readFile(path);
        bool threw = false;
        try {
            file.writeRaw(path.string());
        } catch (const std::exception &) {
            threw = true;
        }
        assert(threw);
        assert(readFile(path) == before);  // never touched
        std::cout << "case 5 (invalid reassembled bytes -> writeRaw() refuses, nothing written) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
