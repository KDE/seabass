// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Release rig: the filesystem repair Library Health offers, end to end, on
// a filesystem this program damages itself -- rig check H1.
//
//   rig_fs_repair [--keep]
//
// No stick is touched and nothing needs root. A FAT32 image is made in a
// temporary directory and attached as a real block device -- udisks2 loop
// devices on Linux, hdiutil on macOS -- so the same code paths run as on a
// stick: the device lookup, the read-only detection, and the platform's
// own repair (udisks2 Filesystem.Repair / diskutil repairVolume).
//
// The damage is deliberate and repairable: FAT32's "cleanly unmounted" bit
// is cleared and the free-cluster summary in FSINFO is wronged, which is
// what an unclean unplug leaves behind and what fsck puts right. No
// directory entry and no file's data is touched, so a repair that works
// leaves every file readable -- which this checks afterwards.
//
// The volume is then mounted read-only, the state a damaged stick reaches
// on its own, so the check is deterministic rather than waiting for the
// kernel to trip over the damage at a moment of its choosing.
//
// Windows is missing on purpose: mounting a VHD needs administrator rights
// and so does Repair-Volume, so that path cannot be a scripted check --
// see the ticket about damaging a real stick from seabass-cli.
//
// The last line is "RIG RESULT: PASS" or "RIG RESULT: FAIL", and the exit
// code matches.

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "infrastructure/media/filesystem_health.hpp"
#include "infrastructure/process/run_command.hpp"

namespace fs = std::filesystem;
using seabass::infrastructure::media::isMountedReadOnly;
using seabass::infrastructure::media::isMountPointRoot;
using seabass::infrastructure::media::repairFilesystem;
using seabass::infrastructure::process::runCommand;

namespace
{

bool failed = false;

void check(bool condition, const std::string &what)
{
    std::cout << (condition ? "  ok   " : "  FAIL ") << what << "\n";
    if (!condition) {
        failed = true;
    }
}

std::string trimmed(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.')) {
        text.pop_back();
    }
    return text;
}

// The word after `marker` in a command's output -- how both udisksctl and
// hdiutil report what they just attached.
std::string wordAfter(const std::string &output, const std::string &marker)
{
    const auto at = output.find(marker);
    if (at == std::string::npos) {
        return {};
    }
    std::string rest = output.substr(at + marker.size());
    const auto end = rest.find_first_of(" \t\r\n");
    return trimmed(end == std::string::npos ? rest : rest.substr(0, end));
}

struct Attached
{
    std::string device;
    std::string mountPoint;
};

// --- the FAT32 damage -------------------------------------------------
//
// Two faults, both in the filesystem's own bookkeeping rather than in
// anyone's files: FAT[1]'s ClnShutBitMask says the volume was not
// unmounted cleanly, and FSINFO's free-cluster count is a lie. fsck
// reports them as "Dirty bit is set" and "Free cluster summary wrong",
// and fixes both.
bool damage(const fs::path &image)
{
    std::fstream file(image, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        return false;
    }
    std::array<char, 512> boot{};
    file.read(boot.data(), boot.size());
    const auto u16 = [&boot](size_t at) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(boot[at]))
            | (static_cast<std::uint32_t>(static_cast<unsigned char>(boot[at + 1])) << 8);
    };
    const std::uint32_t bytesPerSector = u16(11);
    const std::uint32_t reservedSectors = u16(14);
    const std::uint32_t fsInfoSector = u16(48);
    if (bytesPerSector == 0 || reservedSectors == 0) {
        return false;
    }

    // FAT[1], the second entry of the first FAT: clearing bit 27 is
    // exactly what a FAT driver does when it mounts and has not yet had a
    // clean unmount to set it again.
    const std::streamoff fatOffset = static_cast<std::streamoff>(reservedSectors) * bytesPerSector;
    file.seekg(fatOffset + 4);
    std::array<char, 4> entry{};
    file.read(entry.data(), entry.size());
    entry[3] = static_cast<char>(static_cast<unsigned char>(entry[3]) & 0xF7);  // clear bit 27
    file.seekp(fatOffset + 4);
    file.write(entry.data(), entry.size());

    // FSINFO's free-cluster count and "next free" hint, both nonsense now.
    const std::streamoff fsInfoOffset = static_cast<std::streamoff>(fsInfoSector) * bytesPerSector;
    const std::array<char, 4> wrong{0x07, 0x00, 0x00, 0x00};
    file.seekp(fsInfoOffset + 488);
    file.write(wrong.data(), wrong.size());
    file.seekp(fsInfoOffset + 492);
    file.write(wrong.data(), wrong.size());
    file.flush();
    return static_cast<bool>(file);
}

#if defined(__APPLE__)

bool makeImage(const fs::path &image)
{
    // -fs MS-DOS makes FAT32 at this size, and a raw image is what can be
    // edited byte for byte afterwards.
    const auto made = runCommand({"hdiutil", "create", "-size", "64m", "-fs", "MS-DOS", "-volname", "RIGFS",
                                  "-layout", "NONE", "-type", "UDIF", "-ov", image.string()});
    if (made.exitCode != 0) {
        return false;
    }
    // hdiutil appends .dmg to whatever it is given, so asking for
    // "rigfs.img" produces "rigfs.img.dmg". Everything after this --
    // damaging the bytes, attaching, repairing -- uses the path that was
    // asked for, so put the file there.
    //
    // This is why H1 had never passed on macOS: creation reported success
    // and the attach that followed said "No such file or directory" about
    // a file one suffix away.
    std::error_code ec;
    const fs::path withSuffix = image.string() + ".dmg";
    if (!fs::exists(image, ec) && fs::exists(withSuffix, ec)) {
        fs::rename(withSuffix, image, ec);
        if (ec) {
            std::cout << "could not rename " << withSuffix << " to " << image << ": " << ec.message() << "\n";
            return false;
        }
    }
    return fs::exists(image, ec);
}

Attached attach(const fs::path &image, bool readOnly)
{
    std::vector<std::string> command{"hdiutil", "attach", "-imagekey", "diskimage-class=CRawDiskImage", "-nobrowse"};
    if (readOnly) {
        command.push_back("-readonly");
    }
    command.push_back(image.string());
    const auto attached = runCommand(command);
    if (attached.exitCode != 0) {
        std::cout << attached.output << "\n";
        return {};
    }
    Attached result;
    // "/dev/disk4s1  DOS_FAT_32  /Volumes/RIGFS"
    const auto at = attached.output.find("/dev/disk");
    if (at != std::string::npos) {
        std::string rest = attached.output.substr(at);
        result.device = rest.substr(0, rest.find_first_of(" \t"));
    }
    const auto mounted = attached.output.find("/Volumes/");
    if (mounted != std::string::npos) {
        result.mountPoint = trimmed(attached.output.substr(mounted, attached.output.find('\n', mounted) - mounted));
    }
    return result;
}

void detach(const Attached &attached)
{
    if (!attached.device.empty()) {
        runCommand({"hdiutil", "detach", attached.device, "-force"});
    }
}

#else

bool makeImage(const fs::path &image)
{
    {
        std::ofstream out(image, std::ios::binary);
        out.seekp(64 * 1024 * 1024 - 1);
        out.put('\0');
        if (!out) {
            return false;
        }
    }
    const auto formatted = runCommand({"mkfs.vfat", "-F", "32", "-n", "RIGFS", image.string()});
    if (formatted.exitCode != 0) {
        std::cout << formatted.output << "\n";
    }
    return formatted.exitCode == 0;
}

Attached attach(const fs::path &image, bool readOnly)
{
    Attached result;
    // udisks2 sets the loop device up for this user, which is what keeps
    // the whole check root-free -- and it is the same service the repair
    // itself goes through.
    const auto looped = runCommand({"udisksctl", "loop-setup", "-f", image.string(), "--no-user-interaction"});
    if (looped.exitCode != 0) {
        std::cout << looped.output << "\n";
        return result;
    }
    result.device = wordAfter(looped.output, " as ");
    if (result.device.empty()) {
        std::cout << "could not read the loop device out of: " << looped.output << "\n";
        return result;
    }
    std::vector<std::string> command{"udisksctl", "mount", "-b", result.device, "--no-user-interaction"};
    if (readOnly) {
        command.push_back("-o");
        command.push_back("ro");
    }
    const auto mounted = runCommand(command);
    if (mounted.exitCode != 0) {
        std::cout << mounted.output << "\n";
        return result;
    }
    result.mountPoint = wordAfter(mounted.output, " at ");
    return result;
}

void detach(const Attached &attached)
{
    if (attached.device.empty()) {
        return;
    }
    runCommand({"udisksctl", "unmount", "-b", attached.device, "--no-user-interaction"});
    runCommand({"udisksctl", "loop-delete", "-b", attached.device, "--no-user-interaction"});
}

#endif

}  // namespace

int main(int argc, char **argv)
{
    const bool keep = argc > 1 && std::string(argv[1]) == "--keep";
    std::error_code ec;
    const fs::path work = fs::temp_directory_path(ec) / "seabass-rig-fs-repair";
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    const fs::path image = work / "rigfs.img";

    std::cout << "A FAT32 image, damaged on purpose, repaired through the same call Library Health makes.\n";
    if (!makeImage(image)) {
        std::cout << "could not make the image\nRIG RESULT: FAIL\n";
        return 1;
    }

    // Files to prove the repair keeps what was on it.
    Attached attached = attach(image, false);
    if (attached.mountPoint.empty()) {
        std::cout << "could not attach the image\nRIG RESULT: FAIL\n";
        return 1;
    }
    for (int i = 0; i < 8; ++i) {
        std::ofstream out(fs::path(attached.mountPoint) / ("track-" + std::to_string(i) + ".bin"), std::ios::binary);
        out << std::string(64 * 1024, static_cast<char>('a' + i));
    }
    detach(attached);

    check(damage(image), "the image can be damaged (dirty bit, wrong free-cluster summary)");

    attached = attach(image, true);
    if (attached.mountPoint.empty()) {
        std::cout << "could not attach the damaged image read-only\nRIG RESULT: FAIL\n";
        return 1;
    }
    std::cout << "attached " << attached.device << " at " << attached.mountPoint << "\n";
    check(isMountPointRoot(attached.mountPoint), "the mount point is a filesystem of its own, so a repair may run");
    check(isMountedReadOnly(attached.mountPoint), "Seabass sees the volume as read-only");

    const auto result = repairFilesystem(attached.mountPoint);
    std::cout << "repair said: " << result.message << "\n";
    check(!result.declined, "the repair was not declined (a permission prompt may have asked)");
    check(result.repaired, "the repair reports the filesystem consistent again");
    check(!isMountedReadOnly(attached.mountPoint), "and Seabass no longer sees it as read-only");

    int found = 0;
    for (int i = 0; i < 8; ++i) {
        std::ifstream in(fs::path(attached.mountPoint) / ("track-" + std::to_string(i) + ".bin"), std::ios::binary);
        if (in && in.seekg(0, std::ios::end).tellg() == 64 * 1024) {
            found++;
        }
    }
    check(found == 8, "every file written before the damage is still there and whole (" + std::to_string(found)
                          + " of 8)");

    detach(attached);
    if (!keep) {
        fs::remove_all(work, ec);
    } else {
        std::cout << "kept " << work << "\n";
    }
    std::cout << (failed ? "RIG RESULT: FAIL\n" : "RIG RESULT: PASS\n");
    return failed ? 1 : 0;
}
