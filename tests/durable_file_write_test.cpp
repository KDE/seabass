// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "infrastructure/durable_file_write.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure;
namespace fs = std::filesystem;

namespace
{

std::string readFile(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void writeFile(const fs::path &p, const std::string &content)
{
    std::ofstream out(p, std::ios::binary);
    out << content;
}

}  // namespace

int main()
{
    fs::path root = seabass::testing::scratchRoot() / "seabass_durable_file_write_test";
    fs::remove_all(root);
    fs::create_directories(root);

    // Replaces an existing target's content, and leaves no temp file behind.
    {
        fs::path target = root / "target.db";
        writeFile(target, "old content");
        fs::path source = root / "new1.db";
        writeFile(source, "new content, replacing the old");

        bool ok = copyFileDurablyAtomic(source.string(), target.string());
        assert(ok);
        assert(readFile(target) == "new content, replacing the old");

        int strayTempFiles = 0;
        for (const auto &entry : fs::directory_iterator(root)) {
            if (entry.path().filename().string().find(".tmp-") != std::string::npos) {
                strayTempFiles++;
            }
        }
        assert(strayTempFiles == 0);
        std::cout << "case 1 (replaces existing target, no leftover temp file) OK\n";
    }

    // Target doesn't exist yet -- first-time create case.
    {
        fs::path target = root / "brand_new.db";
        fs::path source = root / "new2.db";
        writeFile(source, "first write");

        bool ok = copyFileDurablyAtomic(source.string(), target.string());
        assert(ok);
        assert(readFile(target) == "first write");
        std::cout << "case 2 (target didn't exist yet) OK\n";
    }

    // Source doesn't exist -- target (if any) is left completely untouched.
    {
        fs::path target = root / "untouched.db";
        writeFile(target, "must survive");

        bool ok = copyFileDurablyAtomic((root / "does_not_exist.db").string(), target.string());
        assert(!ok);
        assert(readFile(target) == "must survive");
        std::cout << "case 3 (missing source leaves target untouched) OK\n";
    }

    // Simulates the real crash scenario this function exists for: a
    // previous run wrote (or was writing) the ".tmp-seabass-write" file
    // and never got to fsync/rename before the stick was pulled or the
    // process died, leaving that stale/partial temp file behind. The
    // *next* run must not get confused by it -- writeFileDurablyAtomic()
    // opens its temp path with O_TRUNC, so it should just overwrite the
    // garbage and complete correctly, leaving both a correct target and
    // no leftover temp file, exactly as if the stale file had never
    // existed.
    {
        fs::path target = root / "recovers_from_stale_temp.db";
        writeFile(target, "old content");
        fs::path staleTemp = fs::path(target.string() + ".tmp-seabass-write");
        writeFile(staleTemp, "garbage left over from a previous interrupted write, wrong length too");
        assert(fs::exists(staleTemp));

        bool ok = writeFileDurablyAtomic(target.string(), "correct new content");
        assert(ok);
        assert(readFile(target) == "correct new content");
        assert(!fs::exists(staleTemp));  // consumed by the write, not left behind again
        std::cout << "case 4 (recovers cleanly from a stale temp file left by a previous interrupted write) OK\n";
    }

    // The core safety property, forced rather than just architecturally
    // argued: if the write to the temp file can't even be attempted
    // (simulated here via a read-only containing directory, standing in
    // for "disk full"/"pulled mid-write"/any failure before the atomic
    // rename), the target's existing content must survive completely
    // untouched -- not truncated, not partially overwritten, not
    // deleted -- since the real target is never opened at all until the
    // rename that only happens after a fully-successful temp-file write.
#ifndef _WIN32
    {
        fs::path lockedDir = root / "locked_for_write";
        fs::create_directories(lockedDir);
        fs::path target = lockedDir / "protected.db";
        writeFile(target, "must survive an impossible write");
        fs::permissions(lockedDir, fs::perms::owner_read | fs::perms::owner_exec);

        bool ok = writeFileDurablyAtomic(target.string(), "this must never land");

        fs::permissions(lockedDir, fs::perms::owner_all);  // restore so cleanup can remove it below

        assert(!ok);
        assert(readFile(target) == "must survive an impossible write");
        std::cout << "case 5 (temp-file write impossible -> target survives completely untouched) OK\n";
    }
#endif

    fs::remove_all(root);
    std::cout << "all cases passed\n";
    return 0;
}
