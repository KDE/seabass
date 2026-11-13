// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The hidden damage-filesystem command writes eight bytes and no others.
//
// Its guards decide WHETHER to damage a device; these two functions decide
// WHERE, and a wrong offset here is the one mistake that could reach a
// directory entry or a track. Issue #37 is explicit that the damage must
// be the filesystem's own bookkeeping and nothing else, because the
// repair is the thing under test and a stick that cannot be repaired
// would test nothing.
//
// So the claim under test is not "it set the dirty bit". It is "it
// changed exactly these eight bytes, at offsets derived from the volume's
// own boot sector, and left every other byte of the filesystem alone" --
// checked by comparing the whole image before and after, including the
// files written into it.
//
// Two volumes are damaged. One is committed, tests/fixtures/fat32_volume:
// mkfs.fat made it and it is unpacked here, so those cases run on every
// machine and the test has no way to skip. It used to exit 77 without
// mkfs.fat, which is how it sat as "Skipped" on both CI Linux jobs. The
// other is made here by this machine's own mkfs.fat, and both are handed
// to fsck.fat: CMake finds the tools at configure time and refuses to
// configure without them unless SEABASS_REQUIRE_FAT_TOOLS is OFF. A tool
// this build was configured with and that then fails is a failure.

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <zlib.h>

#include "cli/damage_filesystem.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass::cli;

namespace
{

// What CMake found when this build was configured, as absolute paths: the
// CI image keeps /usr/sbin off PATH, so a name alone is not enough. Empty
// when it found nothing.
#ifndef SEABASS_CONFIGURED_MKFS_FAT
#define SEABASS_CONFIGURED_MKFS_FAT ""
#endif
#ifndef SEABASS_CONFIGURED_FSCK_FAT
#define SEABASS_CONFIGURED_FSCK_FAT ""
#endif
#ifndef SEABASS_CONFIGURED_MCOPY
#define SEABASS_CONFIGURED_MCOPY ""
#endif

const std::string mkfsFat = SEABASS_CONFIGURED_MKFS_FAT;
const std::string fsckFat = SEABASS_CONFIGURED_FSCK_FAT;
const std::string mcopy = SEABASS_CONFIGURED_MCOPY;

std::string readAll(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

std::string quoted(const std::string &text)
{
    std::string out = "'";
    for (char c : text) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    return out + "'";
}

std::string quoted(const fs::path &p)
{
    return quoted(seabass::pathToUtf8(p));
}

int run(const std::string &command)
{
    return std::system((command + " > /dev/null 2>&1").c_str());
}

// The committed volume, unpacked. zlib reads the gzip wrapper itself
// (15 + 32), so nothing but the library this already links is needed.
bool unpackCommittedImage(const fs::path &image)
{
    const fs::path packedPath =
        seabass::pathFromUtf8(SEABASS_SOURCE_DIR) / "tests" / "fixtures" / "fat32_volume" / "fat32-64mib.img.gz";
    const std::string packed = readAll(packedPath);
    if (packed.empty()) {
        std::cerr << seabass::pathToUtf8(packedPath) << " is missing or empty\n";
        return false;
    }
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 32) != Z_OK) {
        std::cerr << "zlib would not start\n";
        return false;
    }
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(packed.data()));
    stream.avail_in = static_cast<uInt>(packed.size());
    std::ofstream out(image, std::ios::binary);
    std::vector<char> chunk(1024 * 1024);
    int state = Z_OK;
    while (state == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef *>(chunk.data());
        stream.avail_out = static_cast<uInt>(chunk.size());
        state = inflate(&stream, Z_NO_FLUSH);
        out.write(chunk.data(), static_cast<std::streamsize>(chunk.size() - stream.avail_out));
    }
    inflateEnd(&stream);
    out.close();
    if (state != Z_STREAM_END || !out) {
        std::cerr << seabass::pathToUtf8(packedPath) << " did not unpack (zlib said " << state << ")\n";
        return false;
    }
    return true;
}

// A FAT32 filesystem made on this machine, with files in it when mtools
// is here. The files are not essential: the byte comparison covers the
// whole image either way, and the committed volume always has them.
bool makeFat32Image(const fs::path &image, const fs::path &dir)
{
    {
        std::ofstream out(image, std::ios::binary);
        const std::string zeroes(1024 * 1024, '\0');
        for (int i = 0; i < 64; ++i) {
            out.write(zeroes.data(), static_cast<std::streamsize>(zeroes.size()));
        }
        out.close();
        if (!out) {
            return false;
        }
    }
    if (run(quoted(mkfsFat) + " -F 32 -n DAMAGETEST " + quoted(image)) != 0) {
        return false;
    }
    if (!mcopy.empty()) {
        const fs::path one = dir / "track1.mp3";
        const fs::path two = dir / "track2.mp3";
        std::ofstream(one, std::ios::binary) << "a track";
        std::ofstream(two, std::ios::binary) << "another";
        if (run(quoted(mcopy) + " -i " + quoted(image) + " " + quoted(one) + " ::track1.mp3") != 0
            || run(quoted(mcopy) + " -i " + quoted(image) + " " + quoted(two) + " ::track2.mp3") != 0) {
            return false;
        }
    }
    return true;
}

std::uint32_t fatEntry(const std::string &bytes, size_t at)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at]))
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 16)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3])) << 24);
}

// Exactly eight bytes change, and they are the right eight. Returns how
// many did, having asserted the rest.
size_t damageAndCompare(const fs::path &image)
{
    const std::string before = readAll(image);
    assert(before.size() == 64u * 1024 * 1024);

    std::string why;
    const auto geometry = readFat32Geometry(seabass::pathToUtf8(image), why);
    if (!geometry) {
        std::cerr << "a volume mkfs.fat made was not recognised: " << why << "\n";
    }
    assert(geometry && "mkfs.fat -F 32 must be recognised as FAT32");
    assert(geometry->bytesPerSector > 0 && geometry->reservedSectors > 0);

    assert(applyDamage(seabass::pathToUtf8(image), *geometry, why));
    const std::string after = readAll(image);
    assert(after.size() == before.size() && "the image must not change size");

    std::vector<size_t> changed;
    for (size_t i = 0; i < before.size(); ++i) {
        if (before[i] != after[i]) {
            changed.push_back(i);
        }
    }

    const size_t fatOffset = static_cast<size_t>(geometry->reservedSectors) * geometry->bytesPerSector;
    const size_t fsInfoOffset = static_cast<size_t>(geometry->fsInfoSector) * geometry->bytesPerSector;

    if (changed.size() > 8) {
        std::cerr << changed.size() << " bytes changed, expected at most 8. Offsets:";
        for (size_t i = 0; i < changed.size() && i < 40; ++i) {
            std::cerr << " " << changed[i];
        }
        std::cerr << "\n  FAT[1] is at " << (fatOffset + 4) << ", FSINFO fields at " << (fsInfoOffset + 488)
                  << " and " << (fsInfoOffset + 492) << "\n";
    }
    assert(changed.size() <= 8 && "at most eight bytes, or this is writing somewhere it should not");
    assert(!changed.empty() && "and at least one, or it did nothing at all");

    // Every changed byte must lie in one of the three known fields.
    for (size_t at : changed) {
        const bool inFatEntry = at >= fatOffset + 4 && at < fatOffset + 8;
        const bool inFreeCount = at >= fsInfoOffset + 488 && at < fsInfoOffset + 492;
        const bool inNextFree = at >= fsInfoOffset + 492 && at < fsInfoOffset + 496;
        if (!(inFatEntry || inFreeCount || inNextFree)) {
            std::cerr << "byte " << at << " changed and is in none of the three bookkeeping fields\n";
        }
        assert((inFatEntry || inFreeCount || inNextFree)
               && "every changed byte is FAT[1] or an FSINFO counter, never a directory entry or file data");
    }

    // The dirty bit specifically: bit 27 of FAT[1] cleared.
    assert((fatEntry(before, fatOffset + 4) & 0x08000000u) != 0 && "the volume starts out marked cleanly unmounted");
    assert((fatEntry(after, fatOffset + 4) & 0x08000000u) == 0 && "and is not afterwards");
    return changed.size();
}

// fsck agrees the volume is damaged AND can put it right, which is the
// whole reason for choosing this damage rather than any other.
void fsckReportsThenRepairs(const fs::path &image)
{
    const int dirty = run(quoted(fsckFat) + " -n " + quoted(image));
    assert(dirty != 0 && "fsck must report the damage, or the command damaged nothing fsck can see");
    run(quoted(fsckFat) + " -a " + quoted(image));
    const int clean = run(quoted(fsckFat) + " -n " + quoted(image));
    assert(clean == 0 && "and must be able to repair it: that repair is the feature under test");
}

}  // namespace

int main()
{
    const fs::path dir = seabass::testing::scratchRoot() / "seabass_damage_filesystem";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    // ---- a volume that is not FAT32 at all is refused -----------------
    {
        const fs::path notFat = dir / "not-fat.img";
        std::ofstream out(notFat, std::ios::binary);
        const std::string junk(1024 * 1024, '\0');
        out.write(junk.data(), junk.size());
        out.close();

        std::string why;
        assert(!readFat32Geometry(seabass::pathToUtf8(notFat), why) && "a volume with no boot signature is not FAT32");
        assert(why.find("not a FAT volume") != std::string::npos);
        std::cout << "case 1 (no boot signature is refused): " << why << "\n";
    }

    // ---- the committed volume: runs on every machine -------------------
    {
        const fs::path image = dir / "committed.img";
        if (!unpackCommittedImage(image)) {
            return 1;
        }
        // Clean before, or fsck's verdict afterwards would prove nothing.
        if (!fsckFat.empty() && run(quoted(fsckFat) + " -n " + quoted(image)) != 0) {
            std::cerr << fsckFat << " does not call the committed volume clean before anything was damaged\n";
            return 1;
        }
        const size_t changed = damageAndCompare(image);
        std::cout << "case 2 (committed volume: " << changed << " byte(s) changed, all in FAT[1] or FSINFO) OK\n";
        if (!fsckFat.empty()) {
            fsckReportsThenRepairs(image);
            std::cout << "case 3 (committed volume: fsck.fat reports it, then repairs it clean) OK\n";
        }
    }

    // ---- a volume this machine's own mkfs.fat makes --------------------
    if (!mkfsFat.empty()) {
        const fs::path image = dir / "stick.img";
        if (!makeFat32Image(image, dir)) {
            std::cerr << "this build was configured with " << mkfsFat << (mcopy.empty() ? "" : " and " + mcopy)
                      << ", and making a FAT32 image with it failed\n";
            return 1;
        }
        const size_t changed = damageAndCompare(image);
        std::cout << "case 4 (fresh volume, " << (mcopy.empty() ? "empty since mcopy is not here" : "with files in it")
                  << ": " << changed << " byte(s) changed, all in FAT[1] or FSINFO) OK\n";
        if (!fsckFat.empty()) {
            fsckReportsThenRepairs(image);
            std::cout << "case 5 (fresh volume: fsck.fat reports it, then repairs it clean) OK\n";
        }
    } else {
        std::cout << "  mkfs.fat was not found when this build was configured: only the committed volume was damaged\n";
    }
    if (fsckFat.empty()) {
        std::cout << "  fsck.fat was not found when this build was configured: that it reports the damage and\n"
                     "  repairs it was not checked here\n";
    }

    fs::remove_all(dir, ec);
    std::cout << "damage_filesystem_test passed\n";
    return 0;
}
