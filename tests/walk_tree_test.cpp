// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The bug this exists for: a wear check on a real 31.5 GB stick reported
// "Nothing was read." for a stick holding 85 tracks. The walk had stopped at
// /Volumes/A1/.Spotlight-V100 with "Operation not permitted" -- EPERM, which
// recursive_directory_iterator's skip_permission_denied does not cover, only
// EACCES -- and the loop shape `for (; !ec && it != end; it.increment(ec))`
// abandons everything on the first error.
//
// macOS creates that directory on every USB volume it indexes. A disk image
// has no Spotlight index, so the rig never saw it, and neither did any test
// on any developer machine.
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "storageprobe/walk_tree.hpp"

namespace fs = std::filesystem;
using storageprobe::walkTree;

namespace
{

// A fresh directory per case, and torn down with permissions restored
// first: a case that locks a directory on purpose cannot be cleaned up
// otherwise, and the failure lands on the NEXT run, in a different test.
// That happened while writing this.
fs::path scratch(const std::string &name)
{
    const char *tmp = std::getenv("TMPDIR");
    fs::path base = tmp ? fs::path(tmp) : fs::path("/tmp");
    base /= "seabass-walk-tree-test-" + name;
    std::error_code ec;
    fs::permissions(base, fs::perms::owner_all, fs::perm_options::add, ec);
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    return base;
}

void tearDown(const fs::path &root)
{
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
          it.increment(ec)) {
        std::error_code permEc;
        fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, permEc);
    }
    fs::remove_all(root, ec);
}

void write(const fs::path &path, const std::string &content)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

void testFindsEveryFile()
{
    const fs::path root = scratch("finds");
    write(root / "a.mp3", "aaaa");
    write(root / "sub" / "b.mp3", "bbbbbb");
    write(root / "sub" / "deeper" / "c.mp3", "cc");

    auto walk = walkTree(root.string());
    assert(walk.files.size() == 3);
    assert(walk.skipped.empty());

    std::uint64_t bytes = 0;
    for (const auto &file : walk.files) {
        bytes += file.size;
    }
    assert(bytes == 12);
    tearDown(root);
}

void testUnreadableDirectoryCostsThatDirectoryOnly()
{
    // The whole point. An unreadable directory must not cost the walk.
    const fs::path root = scratch("locked");
    write(root / "keeper.mp3", "aaaa");
    write(root / "locked" / "hidden.mp3", "bbbb");
    write(root / "later" / "also-keeper.mp3", "cc");

    fs::permissions(root / "locked", fs::perms::none);

    // Whether the OS actually enforced it, not whether the call reported
    // success: std::filesystem::permissions() on Windows only ever
    // toggles the read-only attribute bit, so perms::none on a directory
    // there does not stop it from being listed, and this test's whole
    // premise -- a directory the walk cannot read -- cannot be
    // constructed. Checked directly, the same way
    // failed_change_rollback_test's cases 6 and 7 check theirs.
    std::error_code listEc;
    fs::directory_iterator(root / "locked", listEc);
    const bool lockEnforced = static_cast<bool>(listEc);

    auto walk = walkTree(root.string());
    if (!lockEnforced) {
        std::cout << "testUnreadableDirectoryCostsThatDirectoryOnly SKIPPED (this filesystem still let "
                     "the locked directory be listed, so an unreadable directory could not be "
                     "constructed)\n";
    } else {
        // Both readable files, from directories on either side of the locked one.
        assert(walk.files.size() == 2);
        assert(walk.skipped.size() == 1);
        assert(walk.skipped.front() == "locked");
    }

    tearDown(root);
}

void testPruning()
{
    const fs::path root = scratch("prune");
    write(root / "keep" / "a.mp3", "aaaa");
    write(root / "skip" / "b.mp3", "bbbb");

    auto walk = walkTree(root.string(), [](const std::string &relative) { return relative != "skip"; });
    assert(walk.files.size() == 1);
    assert(walk.files.front().path.find("keep") != std::string::npos);
    // Pruned on purpose is not the same as could not be opened.
    assert(walk.skipped.empty());
    tearDown(root);
}

void testMissingRootIsReportedNotThrown()
{
    auto walk = walkTree("/definitely/not/here");
    assert(walk.files.empty());
    assert(walk.skipped.size() == 1);
}

}  // namespace

int main()
{
    testFindsEveryFile();
    testUnreadableDirectoryCostsThatDirectoryOnly();
    testPruning();
    testMissingRootIsReportedNotThrown();
    std::cout << "walk_tree_test passed\n";
    return 0;
}
