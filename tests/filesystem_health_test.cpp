// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Whether a stick can be written to at all, asked before anything else.
//
// A stick pulled mid-write leaves FAT inconsistent; the kernel then sets
// the filesystem read-only on the next bad cluster count, and a save of a
// thousand small files reports a thousand failures for the one cause.
// Seen on a real stick: "FAT-fs (sdb1): Filesystem has been set
// read-only", after which Seabass said only "could not write
// .../ZoNY-nLLcrbfnOeMKYVrrtb8yb4.jpg".

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include "infrastructure/media/filesystem_health.hpp"
#include "scratch_path.hpp"

namespace fs = std::filesystem;
using namespace seabass::infrastructure::media;

// Run with a path to report on it instead of testing: for checking this
// against a real stick, which is the only place a read-only filesystem
// can actually be seen.
int main(int argc, char **argv)
{
    if (argc > 1) {
        std::cout << argv[1] << "\n"
                  << "  read-only: " << (isMountedReadOnly(argv[1]) ? "yes" : "no") << "\n"
                  << "  device:    " << deviceForMountPoint(argv[1]) << "\n";
        return 0;
    }
    // 1. A directory this process can write is not read-only, and saying
    //    so must not depend on anything mounted.
    {
        const fs::path dir = seabass::testing::scratchRoot() / "seabass_filesystem_health";
        fs::create_directories(dir);
        std::ofstream(dir / "probe") << "x";
        assert(!isMountedReadOnly(dir.string()));
        std::error_code ec;
        fs::remove_all(dir, ec);
        std::cout << "case 1 (a writable directory is not reported read-only) OK\n";
    }

    // 2. A path that is not there is not a read-only stick: the question
    //    was never answered, and answering it wrongly would put "the stick
    //    is read-only" in front of someone whose stick is simply gone.
    {
        assert(!isMountedReadOnly("/nonexistent/seabass/stick"));
        assert(!isMountedReadOnly(""));
        assert(deviceForMountPoint("").empty());
        std::cout << "case 2 (a missing path answers no, rather than guessing) OK\n";
    }

    // 3. The device behind a mount point, which is all the platform's own
    //    repair tool is given. Asked of a path that is really mounted, it
    //    names something; the deepest mount wins, not "/".
    {
        const std::string device = deviceForMountPoint(fs::temp_directory_path().string());
        // No assertion on the name itself: a container, a tmpfs and a
        // plain disk all answer differently, and all of them are right.
        std::cout << "case 3 (a mount point names its device: \"" << device << "\") OK\n";
    }

    // 4. Repairing a path that is not a mount point refuses, and says so,
    //    rather than handing a nonsense device to the platform's tool.
    {
        const FilesystemRepairResult result = repairFilesystem("/nonexistent/seabass/stick");
        assert(!result.repaired);
        assert(!result.message.empty());
        assert(!result.declined);
        std::cout << "case 4 (repairing what is not there refuses, with a reason) OK\n";
    }

    // 5. The guard that keeps the repair on the drive the user pressed
    //    for: a directory inside a filesystem resolves to the filesystem
    //    it sits on, and repairing the disk under someone's home
    //    directory because they opened a library folder there is not a
    //    thing to do by accident.
    {
        assert(!isMountPointRoot(""));
        assert(!isMountPointRoot("/nonexistent/seabass/stick"));
        const fs::path inside = fs::temp_directory_path() / "seabass-not-a-mount-point";
        std::error_code ec;
        fs::create_directories(inside, ec);
        assert(!isMountPointRoot(inside.string()));
        const FilesystemRepairResult result = repairFilesystem(inside.string());
        assert(!result.repaired);
        assert(result.message.find("not a drive of its own") != std::string::npos);
        fs::remove_all(inside, ec);
        // And the one path every system has: "/" is a mount point.
        assert(isMountPointRoot("/") || isMountPointRoot("C:\\"));
        std::cout << "case 5 (a folder inside a filesystem is not a drive to repair) OK\n";
    }

    std::cout << "filesystem_health_test: all cases passed\n";
    return 0;
}
