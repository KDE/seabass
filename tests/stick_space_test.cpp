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
        // .DAT counts too. This case used to assert it did not, on the
        // same stale claim the production comment carried: "no write
        // path here touches it". rekordbox_cue_writer.cpp has written
        // the .DAT's legacy PCOB list since 5282555e (2026-09-18), for
        // hot cues 1-3 and the memory cues, because cues written only
        // into PCO2 are invisible to XDJ-RX2-era players (issue #33).
        //
        // The test and the code agreed with each other and both were
        // wrong, which is why nothing caught it: the estimate was
        // missing roughly half the analysis bytes on a real stick, in
        // the direction that tells the user a backup fits.
        writeFile(root / "PIONEER" / "USBANLZ" / "P002" / "ANLZ0000.DAT", 7000);
        // Audio is not backed up by a cue save at all.
        writeFile(root / "Contents" / "track.mp3", 900000);
        writeFile(root / "PIONEER" / "rekordbox" / "export.pdb", 1000);

        StickSpace measured = measureStickSpace(root);
        assert(measured.capacityBytes > 0);
        assert(measured.worstCaseBackupBytes == 3000 + 5000 + 7000 + 1000);
        std::cout << "case 4 (analysis files of both kinds and catalogs counted, audio not) OK\n";

        fs::remove_all(root);
    }

    // NOT TESTED HERE, deliberately: that one unexaminable entry does
    // not end the walk.
    //
    // I wrote a case for it -- a symlink loop between two analysis files,
    // asserting both are still counted -- and it passed against the bug.
    // recursive_directory_iterator's order is unspecified, so when both
    // readable files happen to come before the loop they are counted
    // before the shared error_code ends the walk, and the assertion
    // holds for a reason that has nothing to do with the fix. Adding
    // more files only lowers the odds of a false pass; it does not
    // remove them, and a case that is right most of the time is the kind
    // this project has spent the day removing.
    //
    // The fix is still right: the iterator's error_code is the
    // iterator's, and the body's calls get their own. It is untested
    // because I could not find a way to test it that did not depend on
    // readdir order.

    std::cout << "all cases passed\n";
    return 0;
}
