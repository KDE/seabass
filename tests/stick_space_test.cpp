// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The rule that decides where a save's backup goes.
//
// The interesting part is not the free-space read -- it is that the size
// of the backup is in the comparison. A single backup can be larger than
// any fixed threshold: RV2 carries 1992 tracks and 318 MB of analysis
// files, and a ten-thousand-track library is about 1.6 GB. A rule that
// only asked "is the stick low" would say yes on an empty 2 TB drive and
// no on a stick that cannot hold the backup at all.

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "infrastructure/backup/stick_space.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::backup;
namespace fs = std::filesystem;

namespace
{

constexpr std::uint64_t Gb = 1024ull * 1024ull * 1024ull;
constexpr std::uint64_t Mb = 1024ull * 1024ull;

void writeFile(const fs::path &path, std::size_t bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << std::string(bytes, 'x');
}

StickSpace at(std::uint64_t capacity, std::uint64_t free, std::uint64_t worstCase)
{
    StickSpace s;
    s.capacityBytes = capacity;
    s.freeBytes = free;
    s.worstCaseBackupBytes = worstCase;
    return s;
}

}  // namespace

int main()
{
    // ---- the rule ------------------------------------------------------
    {
        // Nothing measured: say nothing. Every caller that has not been
        // taught to supply these yet must stay silent rather than warn.
        assert(!at(0, 0, 0).backupGoesLocal());

        // A 30 GB stick with room to spare.
        assert(!at(30 * Gb, 20 * Gb, 318 * Mb).backupGoesLocal());

        // The same stick nearly full: 1.2 GB free, 812 MB to back up
        // leaves 388 MB, under the 1 GB headroom.
        assert(at(30 * Gb, 1200 * Mb, 812 * Mb).backupGoesLocal());

        // Free space alone would have said this stick is fine.
        assert(at(30 * Gb, 1200 * Mb, 1 * Mb).backupGoesLocal() == false);
        std::cout << "case 1 (the backup's own size is in the comparison) OK\n";
    }

    {
        // 2% of a 256 GB stick is 5.1 GB, so 3 GB free is tight there even
        // though it would be plenty on a 30 GB one.
        assert(at(256 * Gb, 3 * Gb, 100 * Mb).backupGoesLocal());
        assert(!at(30 * Gb, 3 * Gb, 100 * Mb).backupGoesLocal());
        std::cout << "case 2 (headroom scales with the device) OK\n";
    }

    {
        // A backup larger than the free space, with no underflow: the
        // subtraction in the rule is on unsigned values, so getting this
        // wrong would wrap and report plenty of room.
        assert(at(30 * Gb, 100 * Mb, 2 * Gb).backupGoesLocal());
        assert(at(30 * Gb, 100 * Mb, 100 * Mb).backupGoesLocal());
        std::cout << "case 3 (a backup bigger than the free space does not wrap) OK\n";
    }

    // ---- the measurement ------------------------------------------------
    {
        const fs::path root = seabass::testing::scratchRoot() / "seabass-stick-space-test";
        fs::remove_all(root);

        // An absent stick measures as nothing rather than throwing.
        StickSpace missing = measureStickSpace(root / "not-there");
        assert(missing.capacityBytes == 0 && missing.worstCaseBackupBytes == 0);
        assert(!missing.backupGoesLocal());

        writeFile(root / "PIONEER" / "USBANLZ" / "P001" / "ANLZ0000.EXT", 3000);
        writeFile(root / "PIONEER" / "USBANLZ" / "P002" / "ANLZ0000.EXT", 5000);
        // .DAT is analysis too, but no write path here touches it, so
        // counting it would overstate the worst case.
        writeFile(root / "PIONEER" / "USBANLZ" / "P002" / "ANLZ0000.DAT", 900000);
        // Audio is not backed up by a cue save at all.
        writeFile(root / "Contents" / "track.mp3", 900000);
        writeFile(root / "PIONEER" / "rekordbox" / "export.pdb", 1000);

        StickSpace measured = measureStickSpace(root);
        assert(measured.capacityBytes > 0);
        assert(measured.worstCaseBackupBytes == 3000 + 5000 + 1000);
        std::cout << "case 4 (analysis files and catalogs counted, .DAT and audio not) OK\n";

        fs::remove_all(root);
    }

    std::cout << "all cases passed\n";
    return 0;
}
