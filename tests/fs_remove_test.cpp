// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// removeEntry() answers "is it gone", where std::filesystem::remove()
// answers "did I unlink something". The difference is what let an exact
// restore on macOS report 42 folders removed that were still on the
// stick: its exFAT driver spells a name one way for a directory read and
// takes only the other for unlink, and remove() reports the name it
// cannot resolve as a quiet false with no error -- the same answer it
// gives for a file that was already deleted.
//
// The decomposed-name case cannot be provoked on the filesystem a test
// runs on (APFS and ext4 both unlink either spelling happily), so what is
// pinned here is the contract every caller depends on: gone is true,
// still-there is false, and false always carries a reason fit to show a
// person.

#include "infrastructure/fs_remove.hpp"

#include "scratch_path.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using seabass::infrastructure::removeEntry;

namespace
{

void writeFile(const fs::path &path, const std::string &content)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "fs-remove-test";
    fs::remove_all(root);
    fs::create_directories(root);

    {
        const fs::path file = root / "plain.mp3";
        writeFile(file, "x");
        std::string failure = "untouched";
        assert(removeEntry(file, failure));
        assert(!fs::exists(file));
        assert(failure == "untouched");  // set only when the entry is still there
        std::cout << "case 1 (an ordinary file goes, and nothing is reported) OK\n";
    }

    {
        // Already gone is gone. A caller counting removals must not be
        // told this one failed, and must not be told a reason either.
        std::string failure;
        assert(removeEntry(root / "never-existed.mp3", failure));
        assert(failure.empty());
        std::cout << "case 2 (an absent entry counts as gone) OK\n";
    }

    {
        // Still there afterwards is a failure, with a reason. A non-empty
        // directory is the case an exact restore hits when a child it
        // could not remove keeps its parent alive.
        const fs::path directory = root / "not empty";
        writeFile(directory / "child.mp3", "x");
        std::string failure;
        assert(!removeEntry(directory, failure));
        assert(fs::is_directory(directory));
        assert(!failure.empty());
        assert(failure != std::error_code().message());  // never "operation completed successfully"
        std::cout << "case 3 (a non-empty folder stays, and says why) OK\n";
    }

    {
        // A decomposed name on a filesystem that unlinks either spelling:
        // the first attempt already works, and the composed retry is
        // never reached. What this pins is that the retry does not get in
        // the way of the ordinary path.
        const fs::path file = root / "Ben Böhmer.mp3";
        writeFile(file, "x");
        std::string failure;
        assert(removeEntry(file, failure));
        std::error_code ec;
        assert(!fs::exists(fs::symlink_status(file, ec)));
        std::cout << "case 4 (a decomposed name goes, on this filesystem too) OK\n";
    }

    {
        // A remove that fails for a real reason must say so, and must not
        // reach the composed-spelling retry: no other spelling of the
        // name would help, and trying one can only put a second entry in
        // the way of a delete. A parent nobody may write is the portable
        // way to provoke it.
        const fs::path parent = root / "locked";
        const fs::path file = parent / "track.mp3";
        writeFile(file, "x");
        fs::permissions(parent, fs::perms::owner_read | fs::perms::owner_exec);
        std::string failure;
        const bool gone = removeEntry(file, failure);
        fs::permissions(parent, fs::perms::owner_all);  // before asserting, or nothing can clean up
        assert(!gone);
        assert(fs::exists(file));
        assert(!failure.empty());
        assert(failure != std::error_code().message());
        std::cout << "case 5 (a refused delete is reported, not counted) OK\n";
    }

    {
        // An entry that is demonstrably still there after the call is a
        // failure, whatever the filesystem answered -- that is the whole
        // contract. Then, emptied, it goes: the deepest-first order an
        // exact restore removes extras in.
        const fs::path directory = root / "full";
        writeFile(directory / "a.mp3", "x");
        writeFile(directory / "b.mp3", "x");
        std::string failure;
        assert(!removeEntry(directory, failure));
        assert(fs::is_directory(directory));
        assert(!failure.empty());
        std::string childFailure;
        assert(removeEntry(directory / "a.mp3", childFailure));
        assert(removeEntry(directory / "b.mp3", childFailure));
        assert(removeEntry(directory, childFailure));
        assert(!fs::exists(directory));
        std::cout << "case 6 (still there is a failure; emptied, it goes) OK\n";
    }

    fs::remove_all(root);
    std::cout << "all cases passed\n";
    return 0;
}
