// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/media/linux_usb_formatter.hpp"

#include <filesystem>

#include "infrastructure/process/run_command.hpp"

namespace seabass::infrastructure::media
{

namespace fs = std::filesystem;
using process::runCommand;

namespace
{

std::string blockObjectPath(const std::string &wholeDiskPath)
{
    // udisks2's own naming convention for block device objects: the
    // kernel device basename, verified by inspecting a real object
    // ("/dev/sdb" <-> "/org/freedesktop/UDisks2/block_devices/sdb") via
    // `gdbus introspect` on this dev machine rather than assumed from
    // documentation alone.
    return "/org/freedesktop/UDisks2/block_devices/" + fs::path(wholeDiskPath).filename().string();
}

// Wraps a string as a quoted GVariant text-format string literal --
// `gdbus call`'s own argument syntax, not a shell (this project never
// shells out through /bin/sh -- see RunCommand's own comment), so this is
// about satisfying GVariant's text grammar, not escaping shell
// metacharacters. Verified against real GVariant parsing (GLib.Variant
// .parse) that a bare unquoted word like `dos` is rejected outright
// ("unknown keyword") -- every string argument gdbus call receives must
// be single-quoted text, including simple identifiers.
std::string gvariantString(const std::string &s)
{
    std::string escaped;
    escaped.reserve(s.size());
    for (char c : s) {
        if (c == '\\' || c == '\'') {
            escaped.push_back('\\');
        }
        escaped.push_back(c);
    }
    return "'" + escaped + "'";
}

std::string formatTypeName(domain::UsbFilesystem fs)
{
    return fs == domain::UsbFilesystem::Fat32 ? "vfat" : "exfat";
}

}  // namespace

// The MBR partition-type byte to stamp on the partition itself.
//
// This used to be left empty, on the documented understanding that udisks2
// would "pick a sensible type for the requested filesystem automatically".
// It does not. A stick formatted FAT32 by this code came back with a real
// mkfs.fat FAT32 filesystem inside a partition still typed 0x83, Linux --
// which Linux never notices, because it probes the content and ignores the
// byte, and macOS refuses outright: it trusts the byte, finds a filesystem
// it has no driver for, and offers to erase the stick. A DJ who says yes
// loses the library.
//
// 0x0c is W95 FAT32 (LBA), which is what every tool that writes a FAT32
// stick for this kind of hardware uses -- diskutil's own MBRFormat FAT32
// on macOS writes it, and so does Windows. 0x07 covers exFAT (shared with
// NTFS/IFS, as the spec intends).
std::string mbrPartitionType(domain::UsbFilesystem fs)
{
    return fs == domain::UsbFilesystem::Fat32 ? "0x0c" : "0x07";
}

std::optional<std::uint64_t> LinuxUsbFormatter::maxSizeFor(domain::UsbFilesystem) const
{
    // mkfs.vfat/mkfs.exfat (what udisks2 shells out to internally) have no
    // artificial size ceiling for either filesystem on Linux -- unlike
    // Windows' Format-Volume, which refuses to create FAT32 over 32GB.
    return std::nullopt;
}

bool LinuxUsbFormatter::format(const std::string &wholeDiskPath, domain::UsbFilesystem fsType,
                                const std::string &volumeLabel, std::string &errorMessage,
                                application::ProgressReporter &progress)
{
    progress.start("Formatting " + wholeDiskPath, 0);

    const std::string objectPath = blockObjectPath(wholeDiskPath);

    // Step 1: wipe whatever's there and lay down a fresh MBR ("dos")
    // partition table -- org.freedesktop.UDisks2.Block.Format(type,
    // options), verified against this D-Bus method's real signature via
    // `gdbus introspect` (see the plan). Every DJ hardware target this
    // project cares about requires MBR, never GPT.
    auto tableResult = runCommand({
        "gdbus",
        "call",
        "--system",
        "--dest",
        "org.freedesktop.UDisks2",
        "--object-path",
        objectPath,
        "--method",
        "org.freedesktop.UDisks2.Block.Format",
        gvariantString("dos"),
        "{}",
    });
    if (tableResult.exitCode != 0) {
        errorMessage = tableResult.output.empty() ? "Failed to create a partition table" : tableResult.output;
        progress.finish();
        return false;
    }

    // Step 2: create the one primary partition spanning the whole disk
    // and format it, in the same call --
    // org.freedesktop.UDisks2.PartitionTable.CreatePartitionAndFormat(
    //   offset, size, type, name, options, format_type, format_options).
    // offset=0/size=0 ("uint64 0" -- gdbus call has no per-argument
    // method-signature awareness, so a bare "0" would parse as int32 and
    // be rejected; verified against real GVariant parsing, not assumed)
    // means "start at the beginning, use all remaining space."
    //
    // `type` is now given explicitly. It used to be '', on the documented
    // understanding that udisks2 picks a sensible MBR type byte for the
    // requested filesystem; the first real scratch stick this was ever run
    // against came back as FAT32-inside-a-Linux-partition, unreadable on
    // macOS. See mbrPartitionType above. The old comment flagged this exact
    // step as unverified, and it was wrong.
    std::string formatOptions = "{'label': <" + gvariantString(volumeLabel) + ">}";
    auto createResult = runCommand({
        "gdbus",
        "call",
        "--system",
        "--dest",
        "org.freedesktop.UDisks2",
        "--object-path",
        objectPath,
        "--method",
        "org.freedesktop.UDisks2.PartitionTable.CreatePartitionAndFormat",
        "uint64 0",
        "uint64 0",
        gvariantString(mbrPartitionType(fsType)),
        "''",
        "{}",
        gvariantString(formatTypeName(fsType)),
        formatOptions,
    });
    progress.finish();
    if (createResult.exitCode != 0) {
        errorMessage =
            createResult.output.empty() ? "Failed to create and format the partition" : createResult.output;
        return false;
    }
    return true;
}

}  // namespace seabass::infrastructure::media
