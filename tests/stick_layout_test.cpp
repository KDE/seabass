// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "infrastructure/stick_layout.hpp"

#include "scratch_path.hpp"

namespace fs = std::filesystem;
using seabass::infrastructure::catalogPathFor;

namespace
{
void touch(const fs::path &p)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << "x";
}
}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_stick_layout_test";
    fs::remove_all(root);
    fs::create_directories(root);

    const std::string pioneer = (root / "PIONEER").string();
    const std::string engineLib = (root / "Engine Library").string();

    // Case 1: nothing on the stick yet -- every catalog is absent, and
    // absent must read as empty rather than as a path a writer would
    // then create a database at.
    {
        assert(catalogPathFor("rekordbox", pioneer).empty());
        assert(catalogPathFor("engine", pioneer).empty());
        assert(catalogPathFor("onelibrary", pioneer).empty());
        std::cout << "case 1 (empty stick -> no paths) OK\n";
    }

    // Case 2: given one catalog's path, the others are found from it --
    // the whole point, since a cleanup holding the Engine path has to
    // reach rekordbox to remove that catalog's row for the same file.
    {
        touch(root / "PIONEER" / "rekordbox" / "export.pdb");
        assert(catalogPathFor("rekordbox", engineLib) == pioneer);
        assert(catalogPathFor("rekordbox", pioneer) == pioneer);
        std::cout << "case 2 (found from another catalog's path) OK\n";
    }

    // Case 3: rekordbox and OneLibrary share the PIONEER directory but
    // are different files in it. Having one must not imply the other.
    {
        assert(catalogPathFor("onelibrary", pioneer).empty());
        std::cout << "case 3 (export.pdb does not imply exportLibrary.db) OK\n";
    }

    // Case 4: Engine, found by its own database rather than its folder
    // -- an "Engine Library" directory with no m.db in it is not a
    // catalog to write to.
    {
        fs::create_directories(root / "Engine Library" / "Database2");
        assert(catalogPathFor("engine", pioneer).empty());
        touch(root / "Engine Library" / "Database2" / "m.db");
        assert(catalogPathFor("engine", pioneer) == engineLib);
        std::cout << "case 4 (engine needs its database, not just its folder) OK\n";
    }

    // Case 5: an unknown format name is not a path.
    {
        assert(catalogPathFor("traktor", pioneer).empty());
        assert(catalogPathFor("", pioneer).empty());
        std::cout << "case 5 (unknown format -> empty) OK\n";
    }

    // Case 6: no library path at all cannot name a stick.
    {
        assert(catalogPathFor("rekordbox", "").empty());
        std::cout << "case 6 (empty library path -> empty) OK\n";
    }

    fs::remove_all(root);
    std::cout << "all cases passed\n";
    return 0;
}
