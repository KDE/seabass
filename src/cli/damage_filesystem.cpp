// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "cli/damage_filesystem.hpp"

#include "cli/console.hpp"
#include "infrastructure/media/media_factory.hpp"
#include "infrastructure/paths/utf8_path.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace seabass::cli
{

namespace
{

std::string megabytes(std::uint64_t bytes)
{
    return std::to_string(bytes / (1024 * 1024)) + " MB";
}

// The stick as the app itself sees it. Anything not in this list is not a
// stick as far as this command is concerned -- see the header.
std::optional<application::DetectedStick> findDetected(const std::string &devicePath, std::vector<std::string> &alsoSeen)
{
    auto locator = infrastructure::media::createRemovableMediaLocator();
    for (const application::DetectedStick &stick : locator->detect()) {
        if (stick.devicePath == devicePath) {
            return stick;
        }
        if (!stick.devicePath.empty()) {
            alsoSeen.push_back(stick.devicePath + (stick.label.empty() ? "" : "  (" + stick.label + ")"));
        }
    }
    return std::nullopt;
}

// Read from the volume's own boot sector rather than trusted from a
// mount option or a label: this decides where the two writes below land,
// and a wrong answer would put them somewhere that is not bookkeeping.
}  // namespace

std::optional<Fat32Geometry> readFat32Geometry(const std::string &devicePath, std::string &why)
{
    std::ifstream in(seabass::pathFromUtf8(devicePath), std::ios::binary);
    if (!in) {
        why = "cannot open " + devicePath + " for reading (run as a user who may read the raw device)";
        return std::nullopt;
    }
    std::array<char, 512> boot{};
    in.read(boot.data(), boot.size());
    if (in.gcount() != static_cast<std::streamsize>(boot.size())) {
        why = "could not read a full boot sector from " + devicePath;
        return std::nullopt;
    }
    const auto u16 = [&boot](size_t at) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(boot[at]))
            | (static_cast<std::uint32_t>(static_cast<unsigned char>(boot[at + 1])) << 8);
    };
    if (static_cast<unsigned char>(boot[510]) != 0x55 || static_cast<unsigned char>(boot[511]) != 0xAA) {
        why = "no boot-sector signature: this is not a FAT volume";
        return std::nullopt;
    }
    // FAT32 says so in its own type field, and says zero in the 16-bit
    // sector count. Both, because either alone is spoofable by a volume
    // that merely looks the part.
    const std::string type(boot.data() + 82, 8);
    if (type.rfind("FAT32", 0) != 0 || u16(17) != 0) {
        why = "not FAT32 (boot sector says \"" + type + "\"); the damage this writes is FAT32-specific";
        return std::nullopt;
    }
    Fat32Geometry g;
    g.bytesPerSector = u16(11);
    g.reservedSectors = u16(14);
    g.fsInfoSector = u16(48);
    if (g.bytesPerSector == 0 || g.reservedSectors == 0) {
        why = "the boot sector reports a zero sector size or no reserved sectors";
        return std::nullopt;
    }
    return g;
}

// The same two faults tools/rig_fs_repair.cpp applies to a loopback
// image, so what a rig round proves and what a real stick shows are the
// same damage. Nothing outside these eight bytes is written.
bool applyDamage(const std::string &devicePath, const Fat32Geometry &g, std::string &why)
{
    std::fstream file(seabass::pathFromUtf8(devicePath), std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        why = "cannot open " + devicePath + " for writing";
        return false;
    }

    const std::streamoff fatOffset = static_cast<std::streamoff>(g.reservedSectors) * g.bytesPerSector;
    file.seekg(fatOffset + 4);
    std::array<char, 4> entry{};
    file.read(entry.data(), entry.size());
    if (file.gcount() != static_cast<std::streamsize>(entry.size())) {
        why = "could not read FAT[1]";
        return false;
    }
    entry[3] = static_cast<char>(static_cast<unsigned char>(entry[3]) & 0xF7);  // clear bit 27
    file.seekp(fatOffset + 4);
    file.write(entry.data(), entry.size());

    const std::streamoff fsInfoOffset = static_cast<std::streamoff>(g.fsInfoSector) * g.bytesPerSector;
    const std::array<char, 4> wrong{0x07, 0x00, 0x00, 0x00};
    file.seekp(fsInfoOffset + 488);
    file.write(wrong.data(), wrong.size());
    file.seekp(fsInfoOffset + 492);
    file.write(wrong.data(), wrong.size());
    file.flush();
    if (!file) {
        why = "the writes did not complete";
        return false;
    }
    return true;
}

namespace
{

// Whether the kernel has actually marked it read-only, which is the
// precondition the tester came for. Guessing it from the UI afterwards is
// exactly what this command exists to replace.
std::optional<bool> mountedReadOnly(const std::string &mountPoint)
{
    std::ifstream mounts("/proc/self/mounts");
    if (!mounts) {
        return std::nullopt;
    }
    std::string device, target, type, options;
    while (mounts >> device >> target >> type >> options) {
        mounts.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        if (target == mountPoint) {
            return options == "ro" || options.rfind("ro,", 0) == 0 || options.find(",ro,") != std::string::npos;
        }
    }
    return std::nullopt;
}

}  // namespace

int runDamageFilesystemCommand(const std::string &deviceArg, bool confirmedOnCommandLine)
{
#if !defined(__linux__)
    (void)deviceArg;
    (void)confirmedOnCommandLine;
    Console::error("damage-filesystem is Linux only: the corruption recipe is FAT-and-kernel specific. "
                   "On macOS and Windows, damage a stick here and move it, or use the loopback check "
                   "(tools/rig_fs_repair).");
    return 1;
#else
    if (!confirmedOnCommandLine) {
        Console::error("damage-filesystem needs --yes-really-destroy, spelled out, before it will look at a device.");
        return 1;
    }
    if (deviceArg.empty()) {
        Console::error("damage-filesystem needs a partition, e.g. /dev/sdb1");
        return 1;
    }

    std::vector<std::string> alsoSeen;
    const auto stick = findDetected(deviceArg, alsoSeen);
    if (!stick) {
        // Deliberately the only way in. Not "is it removable" asked here,
        // but "is it one of the sticks this app already found", which no
        // internal disk can ever be.
        Console::error(deviceArg + " is not a removable USB partition this machine reports. Refusing.");
        if (alsoSeen.empty()) {
            Console::error("No removable USB partitions were found at all. Plug the stick in and try again.");
        } else {
            Console::error("What was found:");
            for (const std::string &seen : alsoSeen) {
                Console::error("  " + seen);
            }
        }
        return 1;
    }

    std::string why;
    const auto geometry = readFat32Geometry(deviceArg, why);
    if (!geometry) {
        Console::error("Refusing to damage " + deviceArg + ": " + why);
        return 1;
    }

    std::uint64_t capacity = 0;
    std::uint64_t free = 0;
    std::vector<std::string> topLevel;
    if (stick->mounted && !stick->mountPoint.empty()) {
        std::error_code ec;
        const fs::path mountPoint = seabass::pathFromUtf8(stick->mountPoint);
        const auto space = fs::space(mountPoint, ec);
        if (!ec) {
            capacity = space.capacity;
            free = space.available;
        }
        for (const auto &entry : fs::directory_iterator(mountPoint, ec)) {
            topLevel.push_back(seabass::pathToUtf8(entry.path().filename()));
            if (topLevel.size() >= 12) {
                topLevel.push_back("...");
                break;
            }
        }
    }

    // Everything about the thing, before anything is asked, so the answer
    // is given to a described stick rather than to a device node.
    Console::heading("About to damage a real filesystem");
    Console::info("  device      " + deviceArg);
    Console::info("  label       " + (stick->label.empty() ? std::string("(untitled)") : stick->label));
    Console::info("  mounted at  " + (stick->mountPoint.empty() ? std::string("(not mounted)") : stick->mountPoint));
    if (capacity > 0) {
        Console::info("  size        " + megabytes(capacity) + ", " + megabytes(free) + " free");
    }
    if (!topLevel.empty()) {
        std::string listing;
        for (const std::string &name : topLevel) {
            listing += (listing.empty() ? "" : ", ") + name;
        }
        Console::info("  contains    " + listing);
    }
    Console::info("");
    Console::info("This clears the clean-unmount bit and wrongs the free-cluster summary. It does NOT");
    Console::info("touch directory entries or file data, and fsck.fat can put both right: that repair");
    Console::info("is the thing under test. Even so, do not do this to a stick whose contents you want.");
    Console::info("");

    const std::string expected = stick->label.empty() ? "(untitled)" : stick->label;
    Console::info("Type the volume label to confirm: " + expected);
    std::string typed;
    if (!std::getline(std::cin, typed) || typed != expected) {
        Console::error("That does not match. Nothing was written.");
        return 1;
    }

    auto mounter = infrastructure::media::createRemovableMediaMounter();
    if (stick->mounted) {
        // release(), not unmount(): this keeps operating on the same
        // device afterwards, and unmount() signals physical removal.
        std::string mountError;
        if (!mounter->release(deviceArg, mountError)) {
            Console::error("Could not release " + deviceArg + " before writing: " + mountError);
            return 1;
        }
        Console::info("released " + deviceArg);
    }

    if (!applyDamage(deviceArg, *geometry, why)) {
        Console::error("The damage did not land: " + why);
        Console::error("The stick may still be released; mount it again before using it.");
        return 1;
    }
    Console::info("damaged: clean-unmount bit cleared, free-cluster summary wronged");

    std::string mountError;
    const auto mountPoint = mounter->mount(deviceArg, mountError);
    if (!mountPoint) {
        // Not a failure of the damage: a kernel that refuses the volume
        // outright is a harsher outcome than the one wanted, and saying
        // which happened is the whole point of checking.
        Console::warn("Damaged, but it would not mount again: " + mountError);
        Console::warn("Repair it with: fsck.fat -a " + deviceArg);
        return 2;
    }

    const auto readOnly = mountedReadOnly(*mountPoint);
    Console::info("mounted again at " + *mountPoint);
    if (!readOnly) {
        Console::warn("Could not tell from /proc/self/mounts whether it came back read-only.");
        return 2;
    }
    if (*readOnly) {
        Console::info("RESULT: the stick is mounted READ-ONLY. The precondition is reached.");
        return 0;
    }
    // Said plainly rather than left to the tester to notice: this kernel
    // mounted a dirty volume read-write, so whatever is tested next is
    // not being tested against a read-only stick.
    Console::warn("RESULT: it came back READ-WRITE. The filesystem is dirty and fsck will report it,");
    Console::warn("but this kernel did not force read-only, so the read-only path is NOT set up.");
    return 2;
#endif
}

}  // namespace seabass::cli
