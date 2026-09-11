// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>

#include "infrastructure/scratch_dir_guard.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure;
namespace fs = std::filesystem;

namespace
{

fs::path makeScratchWithFile(const std::string &name)
{
    fs::path dir = seabass::testing::scratchRoot() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    std::ofstream(dir / "data.txt") << "content";
    return dir;
}

}  // namespace

int main()
{
    // Case 1: the exact previously-vulnerable pattern -- constructing a
    // temporary ScratchDirGuard and passing it to
    // std::optional<ScratchDirGuard>::emplace(). Before this type was
    // made move-only, this compiled but silently deleted the directory
    // immediately (the temporary argument, holding a COPY of the same
    // path, was destroyed at the end of this statement). It should now
    // either fail to compile with a copyable type, or -- as implemented,
    // move-only -- move-construct correctly, leaving the temporary's
    // path empty so its destructor is a no-op.
    {
        fs::path dir = makeScratchWithFile("seabass_scratch_dir_guard_test_1");
        std::optional<ScratchDirGuard> guard;
        guard.emplace(ScratchDirGuard{dir});

        assert(fs::exists(dir));
        assert(fs::exists(dir / "data.txt"));
        std::cout << "case 1 (emplace with a temporary does not prematurely delete the directory) OK\n";

        guard.reset();
        assert(!fs::exists(dir));
        std::cout << "case 2 (directory removed once the guard is actually destroyed) OK\n";
    }

    // Case 3: the simpler, preferred direct-construction form.
    {
        fs::path dir = makeScratchWithFile("seabass_scratch_dir_guard_test_2");
        {
            ScratchDirGuard guard(dir);
            assert(fs::exists(dir));
        }
        assert(!fs::exists(dir));
        std::cout << "case 3 (direct construction cleans up on scope exit) OK\n";
    }

    // Case 4: removing an already-gone directory is a harmless no-op,
    // not a crash/throw -- covers the "moved-from temporary" no-op path
    // directly.
    {
        fs::path dir = seabass::testing::scratchRoot() / "seabass_scratch_dir_guard_test_nonexistent";
        std::error_code ec;
        fs::remove_all(dir, ec);
        { ScratchDirGuard guard(dir); }
        std::cout << "case 4 (destroying a guard over a nonexistent path is a harmless no-op) OK\n";
    }

    std::cout << "All scratch_dir_guard_test cases passed.\n";
    return 0;
}
