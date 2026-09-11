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
    // means "start at the beginning, use all remaining space." An empty
    // `type` ('') is documented udisks2 behavior for "pick a sensible MBR
    // partition-type byte for the requested filesystem automatically"
    // rather than this project hardcoding one -- NOT independently
    // verified against a real destructive call this session (this dev
    // machine's only spare-looking USB disk is Sebastian's real,
    // populated stick), so treat this specific step as unverified until
    // exercised against a real scratch stick, same as the plan's
    // "Real-hardware verification" section already flags.
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
        "''",
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
