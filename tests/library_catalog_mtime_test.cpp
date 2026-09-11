// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "infrastructure/stick_backup/library_catalog_mtime.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure::stick_backup;
namespace fs = std::filesystem;

namespace
{

void writeFile(const fs::path &p, std::int64_t mtime)
{
    fs::create_directories(p.parent_path());
    {
        std::ofstream out(p, std::ios::binary);
        out << "x";
    }
    fs::last_write_time(p, fromUnixSeconds(mtime));
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "seabass_library_catalog_mtime_test";
    fs::remove_all(root);
    fs::create_directories(root);

    // Nothing there: unknown.
    assert(libraryCatalogModifiedAt(root) == 0);
    assert(libraryCatalogModifiedAt(root / "missing") == 0);

    // Audio does not count; the catalog does.
    writeFile(root / "Contents" / "a.mp3", 1'800'000'000);
    assert(libraryCatalogModifiedAt(root) == 0);
    writeFile(root / "PIONEER" / "rekordbox" / "export.pdb", 1'700'000'000);
    assert(libraryCatalogModifiedAt(root) == 1'700'000'000);

    // The newest of the databases wins, WAL included.
    writeFile(root / "Engine Library" / "Database2" / "m.db", 1'700'000'100);
    assert(libraryCatalogModifiedAt(root) == 1'700'000'100);
    writeFile(root / "Engine Library" / "Database2" / "m.db-wal", 1'700'000'200);
    assert(libraryCatalogModifiedAt(root) == 1'700'000'200);
    writeFile(root / "Engine Library" / "Database2" / "hm.db", 1'700'000'300);
    assert(libraryCatalogModifiedAt(root) == 1'700'000'300);
    // -shm is scratch and never consulted.
    writeFile(root / "Engine Library" / "Database2" / "m.db-shm", 1'900'000'000);
    assert(libraryCatalogModifiedAt(root) == 1'700'000'300);

    fs::remove_all(root);
    std::cout << "All library_catalog_mtime tests passed." << std::endl;
    return 0;
}
