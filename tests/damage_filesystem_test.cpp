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
// Needs mkfs.fat. Without it the FAT32 cases cannot be built and the test
// says so and skips rather than reporting a pass it did not earn.

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "cli/damage_filesystem.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass::cli;

namespace
{

std::string readAll(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

bool haveMkfsFat()
{
    return std::system("command -v mkfs.fat > /dev/null 2>&1") == 0;
}

// A real FAT32 filesystem with real files in it, so "nothing else
// changed" is a claim about a populated volume rather than about zeroes.
bool makeFat32Image(const fs::path &image)
{
    const std::string make = "dd if=/dev/zero of=" + seabass::pathToUtf8(image)
        + " bs=1M count=64 status=none && mkfs.fat -F 32 -n DAMAGETEST " + seabass::pathToUtf8(image) + " > /dev/null 2>&1";
    if (std::system(make.c_str()) != 0) {
        return false;
    }
    // Files, via mcopy when it is there. Not essential -- the byte
    // comparison covers the whole image either way -- so a machine
    // without mtools still runs the real check.
    const std::string copy = "command -v mcopy > /dev/null 2>&1 && "
                             "printf 'a track' > /tmp/.damage_t1 && printf 'another' > /tmp/.damage_t2 && "
                             "mcopy -i " + seabass::pathToUtf8(image) + " /tmp/.damage_t1 ::track1.mp3 > /dev/null 2>&1 && "
                             "mcopy -i " + seabass::pathToUtf8(image) + " /tmp/.damage_t2 ::track2.mp3 > /dev/null 2>&1";
    std::system(copy.c_str());
    std::system("rm -f /tmp/.damage_t1 /tmp/.damage_t2");
    return true;
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

    if (!haveMkfsFat()) {
        std::cerr << "mkfs.fat is not on this machine, so no real FAT32 volume can be built here and the\n"
                     "cases that matter cannot run. Not reporting a pass.\n";
        fs::remove_all(dir, ec);
        return 77;
    }

    // ---- exactly eight bytes change, and they are the right eight -----
    {
        const fs::path image = dir / "stick.img";
        if (!makeFat32Image(image)) {
            std::cerr << "mkfs.fat is present but making the image failed; not reporting a pass\n";
            return 77;
        }
        const std::string before = readAll(image);
        assert(before.size() == 64u * 1024 * 1024);

        std::string why;
        const auto geometry = readFat32Geometry(seabass::pathToUtf8(image), why);
        if (!geometry) {
            std::cerr << "a freshly made FAT32 image was not recognised: " << why << "\n";
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
                   && "every changed byte is FAT[1] or an FSINFO counter -- never a directory entry or file data");
        }

        // The dirty bit specifically: bit 27 of FAT[1] cleared.
        const auto fatEntry = [](const std::string &bytes, size_t at) {
            return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at]))
                | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8)
                | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 16)
                | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3])) << 24);
        };
        assert((fatEntry(before, fatOffset + 4) & 0x08000000u) != 0 && "a fresh volume is marked cleanly unmounted");
        assert((fatEntry(after, fatOffset + 4) & 0x08000000u) == 0 && "and is not afterwards");

        std::cout << "case 2 (" << changed.size() << " byte(s) changed, all in FAT[1] or FSINFO) OK\n";

        // And fsck agrees it is damaged AND can put it right -- the whole
        // reason for choosing this damage rather than any other.
        if (std::system("command -v fsck.fat > /dev/null 2>&1") == 0) {
            const int dirty = std::system(("fsck.fat -n " + seabass::pathToUtf8(image) + " > /dev/null 2>&1").c_str());
            assert(dirty != 0 && "fsck must report the damage, or the command damaged nothing fsck can see");
            const int repaired = std::system(("fsck.fat -a " + seabass::pathToUtf8(image) + " > /dev/null 2>&1").c_str());
            (void)repaired;
            const int clean = std::system(("fsck.fat -n " + seabass::pathToUtf8(image) + " > /dev/null 2>&1").c_str());
            assert(clean == 0 && "and must be able to repair it -- that repair is the feature under test");
            std::cout << "case 3 (fsck.fat reports it, then repairs it clean) OK\n";
        } else {
            std::cout << "  fsck.fat not present: the repairable-by-design claim was not checked here\n";
        }
    }

    fs::remove_all(dir, ec);
    std::cout << "damage_filesystem_test passed\n";
    return 0;
}
