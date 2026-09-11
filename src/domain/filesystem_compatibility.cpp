// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/filesystem_compatibility.hpp"

#include <algorithm>
#include <cctype>

namespace seabass::domain
{

namespace
{
std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
}  // namespace

FilesystemCompatibilityInfo FilesystemCompatibility::lookup(const std::string &filesystem)
{
    std::string fs = lower(filesystem);

    if (fs == "vfat" || fs == "fat32" || fs == "msdos") {
        return {
            "FAT32",
            "4 GiB minus 1 byte per file",
            "The most universally supported filesystem across CDJ/XDJ/Engine OS hardware -- this is what "
            "Rekordbox and Engine DJ both format a new USB device library to by default. Its one real "
            "practical limitation is the per-file size cap: a single long, lossless (WAV/AIFF/FLAC) file or "
            "a long recorded mix can exceed 4 GiB and simply won't copy. Splitting long files or using a "
            "compressed format avoids it.",
            true,
        };
    }
    if (fs == "exfat") {
        return {
            "exFAT",
            "16 EiB (no practical limit)",
            "No meaningful file-size limit, so it avoids FAT32's 4 GiB cap. Supported by most current-generation "
            "Pioneer/Denon hardware, but older units and firmware versions have had inconsistent or missing exFAT "
            "support -- check your specific player's manual/firmware notes before relying on it for a gig, "
            "especially on older CDJ/XDJ models.",
            true,
        };
    }
    if (fs == "ntfs") {
        return {
            "NTFS",
            "16 EiB (no practical limit)",
            "Not natively supported for direct playback by CDJ/XDJ hardware as far as this project is aware. "
            "If a stick reports NTFS, it likely isn't set up as a proper Rekordbox/Engine device library and "
            "won't be recognized by a player even though this app can still read/write its own data on it "
            "from a computer.",
            false,
        };
    }
    if (fs == "hfsplus" || fs == "hfs+" || fs == "apfs") {
        return {
            fs == "apfs" ? "APFS" : "HFS+",
            "16 EiB (no practical limit)",
            "A macOS-native filesystem. Not supported for direct playback by CDJ/XDJ hardware -- Rekordbox and "
            "Engine DJ both format device libraries to FAT32/exFAT specifically so players can read them.",
            false,
        };
    }
    if (fs == "ext2" || fs == "ext3" || fs == "ext4") {
        return {
            "ext" + fs.substr(3),
            "16 TiB+ (no practical limit)",
            "A Linux-native filesystem. Not supported for direct playback by any known CDJ/XDJ hardware.",
            false,
        };
    }

    return {
        filesystem.empty() ? "Unknown" : filesystem,
        "Unknown",
        "This filesystem isn't one this project has compatibility notes for. If it's not FAT32 or exFAT, it's "
        "likely not going to be recognized as a device library by CDJ/XDJ hardware.",
        false,
    };
}

}  // namespace seabass::domain
