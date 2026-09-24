// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/backup/stick_space.hpp"

#include <algorithm>

#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::infrastructure::backup
{

namespace fs = std::filesystem;

namespace
{

constexpr std::uint64_t OneGigabyte = 1024ull * 1024ull * 1024ull;

bool isAnalysisFile(const fs::path &path)
{
    // Compared as a path against ASCII literals; a non-ASCII extension is
    // simply neither, so only ASCII letters need folding.
    std::string ext = pathToGenericUtf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::toupper(c); });
    // Both, and the .DAT is not optional any more.
    //
    // This counted only .EXT on the stated grounds that "no write path
    // in Seabass touches it". That stopped being true on 2026-09-18
    // (5282555e): rekordbox_cue_writer.cpp writes the .DAT's legacy PCOB
    // list for hot cues 1-3 and the memory cues, because cues written
    // only into PCO2 are invisible to XDJ-RX2-era players (issue #33).
    //
    // So the worst case was missing roughly half the analysis bytes on a
    // stick, which is what backupGoesLocal() decides against and what
    // EditSessionHost's "Not enough room" dialog quotes at the user.
    // Understating there says a backup fits when it does not.
    return ext == ".EXT" || ext == ".DAT";
}

// The catalogs a save can rewrite whole. Small next to the analysis
// files, but a save that only touches settings backs up exactly these.
std::uint64_t catalogBytes(const fs::path &stickRoot)
{
    static const char *const catalogs[] = {
        "PIONEER/rekordbox/export.pdb",
        "PIONEER/rekordbox/exportExt.pdb",
        "PIONEER/rekordbox/exportLibrary.db",
        "Engine Library/Database2/m.db",
    };
    std::uint64_t total = 0;
    std::error_code ec;
    for (const char *relative : catalogs) {
        const fs::path candidate = stickRoot / relative;
        if (fs::is_regular_file(candidate, ec)) {
            const auto size = fs::file_size(candidate, ec);
            if (!ec) {
                total += size;
            }
        }
    }
    return total;
}

}  // namespace

std::uint64_t StickSpace::headroomBytes() const
{
    return std::max<std::uint64_t>(OneGigabyte, capacityBytes / 50);  // 2%
}

bool StickSpace::backupGoesLocal() const
{
    if (capacityBytes == 0) {
        return false;  // nothing measured, so nothing to warn about
    }
    if (worstCaseBackupBytes >= freeBytes) {
        return true;  // it would not fit at all
    }
    return freeBytes - worstCaseBackupBytes < headroomBytes();
}

StickSpace measureStickSpace(const fs::path &stickRoot)
{
    StickSpace measured;
    std::error_code ec;
    if (stickRoot.empty() || !fs::is_directory(stickRoot, ec)) {
        return measured;
    }

    const auto space = fs::space(stickRoot, ec);
    if (ec) {
        return measured;  // an unreadable stick warns about nothing
    }
    measured.capacityBytes = space.capacity;
    // `available` rather than `free`: the latter counts space only root
    // may use, which nobody here can.
    measured.freeBytes = space.available;

    // Only the analysis tree is walked. It holds thousands of files on a
    // real stick and the whole point of measuring here is that it happens
    // while an edit page opens, so this stays a metadata-only walk of one
    // subtree rather than of the entire device.
    const fs::path analysisRoot = stickRoot / "PIONEER" / "USBANLZ";
    if (fs::is_directory(analysisRoot, ec)) {
        // The walk's error_code is the walk's alone. It used to be shared
        // with the calls in the body, so one entry whose is_regular_file
        // or file_size failed set it, the `!ec` in the loop condition
        // then ended the walk, and everything after that entry went
        // uncounted -- silently, and in the direction that says a backup
        // fits when it does not.
        for (fs::recursive_directory_iterator it(analysisRoot, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) {
            std::error_code entryEc;
            if (!it->is_regular_file(entryEc) || entryEc || !isAnalysisFile(it->path())) {
                continue;
            }
            const auto size = it->file_size(entryEc);
            if (!entryEc) {
                measured.worstCaseBackupBytes += size;
            }
        }
    }
    measured.worstCaseBackupBytes += catalogBytes(stickRoot);
    return measured;
}

}  // namespace seabass::infrastructure::backup
