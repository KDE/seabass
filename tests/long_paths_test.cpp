// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Covers the three things that break on Windows once a path passes the
// classic MAX_PATH of 260 characters. Every case builds a tree ~300
// characters deep with each individual component well under NAME_MAX, so
// the same tree is legal on Linux -- there these cases simply exercise
// long names and pass without exercising the prefix logic at all.

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "infrastructure/long_paths.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"

#include "scratch_path.hpp"

using namespace seabass::infrastructure;
using seabass::application::CancellationToken;
using seabass::infrastructure::stick_backup::TreeEntry;
using seabass::infrastructure::stick_backup::TreeWalk;
using seabass::infrastructure::stick_backup::walkStickTree;
namespace fs = std::filesystem;

namespace
{

// Six 40-character segments under `root`, created one level at a time
// the way a real library grows. Six, not five: under /tmp on Linux five
// stop at 242 characters, short of the 260 the cases below rely on.
// Returns the deepest directory.
fs::path makeDeepTree(const fs::path &root)
{
    std::error_code ec;
    fs::path dir = fs::absolute(root);
    fs::create_directories(dir, ec);
    for (char c = 'a'; c < 'g'; ++c) {
        dir = dir / std::string(40, c);
        fs::create_directories(longPathSafe(dir), ec);
        assert(!ec);
    }
    return dir;
}

void writeThrough(const fs::path &path, const std::string &content)
{
    // Not std::ofstream: it takes the path as given and cannot reach one
    // that needs the prefix.
    std::error_code ec;
    fs::path full = longPathSafe(path);
    std::filesystem::remove(full, ec);
#if defined(_WIN32)
    FILE *f = _wfopen(full.c_str(), L"wb");
#else
    FILE *f = std::fopen(full.c_str(), "wb");
#endif
    assert(f != nullptr);
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
}

fs::path freshRoot(const std::string &name)
{
    fs::path root = seabass::testing::scratchRoot() / ("seabass_long_paths_test_" + name);
    // Deliberately not fs::remove_all -- see the case 1 comment.
    removeTreeDeepestFirst(root);
    std::error_code ec;
    fs::create_directories(root, ec);
    return root;
}

}  // namespace

int main()
{
    // ---- removeTreeDeepestFirst finishes where remove_all cannot ----
    {
        fs::path root = freshRoot("remove");
        fs::path deep = makeDeepTree(root);
        writeThrough(deep / "track.mp3", "x");
        assert(deep.native().size() > 260);
        assert(fs::exists(longPathSafe(deep / "track.mp3")));

        const std::uintmax_t removed = removeTreeDeepestFirst(root);
        // 6 directories + the root + the file.
        assert(removed == 8);
        assert(!fs::exists(longPathSafe(root)));
        assert(!fs::exists(root));
        // Not asserted by running it: fs::remove_all on this same tree
        // never returns on Windows, so a test that called it to compare
        // would hang the suite rather than fail it.
        std::cout << "case 1 (removeTreeDeepestFirst deletes a tree past MAX_PATH and reports the count) OK\n";
    }

    // ---- DirectoryReader lists the directory it was asked about ----
    {
        fs::path root = freshRoot("read");
        fs::path deep = makeDeepTree(root);
        writeThrough(deep / "ONE.txt", "1");
        writeThrough(deep / "TWO.txt", "2");
        assert(deep.native().size() > 260);

        std::error_code ec;
        DirectoryReader reader(deep, ec);
        assert(!ec);
        std::set<std::string> names;
        fs::path child;
        std::error_code readEc;
        while (reader.next(child, readEc)) {
            names.insert(child.filename().string());
        }
        assert(!readEc);
        // The exact-set assertion is the point. std::filesystem's
        // directory_iterator does not fail on a prefixed path -- it
        // silently lists the process's current working directory and
        // reports success, so a "did it find ONE.txt" check would pass
        // against a completely wrong directory whenever the build tree
        // happened to contain a file by that name. Only requiring that
        // nothing else came back catches it.
        assert((names == std::set<std::string>{"ONE.txt", "TWO.txt"}));
        removeTreeDeepestFirst(root);
        std::cout << "case 2 (DirectoryReader past MAX_PATH lists exactly that directory) OK\n";
    }

    // ---- The stick walker descends past MAX_PATH ----
    {
        fs::path root = freshRoot("walk");
        fs::path stick = fs::absolute(root / "stick");
        std::error_code ec;
        fs::create_directories(stick / "Contents", ec);
        writeThrough(stick / "Contents" / "short.mp3", "short");
        fs::path deep = makeDeepTree(stick / "Contents");
        writeThrough(deep / "deep.mp3", std::string(2048, 'D'));
        assert((deep / "deep.mp3").native().size() > 260);

        TreeWalk walk = walkStickTree(stick, CancellationToken::none());
        // Nothing may be reported as unreadable: before DirectoryReader,
        // the deepest directory came back as "No such file or directory"
        // and deep.mp3 was simply absent from the walk -- which reached
        // the user as a warning on a backup that still reported Complete,
        // with the track missing from the archive.
        for (const std::string &skipped : walk.skipped) {
            std::cerr << "unexpectedly skipped: " << skipped << "\n";
        }
        assert(walk.skipped.empty());

        std::set<std::string> paths;
        for (const TreeEntry &entry : walk.entries) {
            paths.insert(entry.relativePath);
        }
        assert(paths.count("Contents/short.mp3") == 1);
        std::string deepRelative = "Contents";
        for (char c = 'a'; c < 'g'; ++c) {
            deepRelative += "/" + std::string(40, c);
        }
        assert(paths.count(deepRelative) == 1);
        assert(paths.count(deepRelative + "/deep.mp3") == 1);
        for (const TreeEntry &entry : walk.entries) {
            if (entry.relativePath == deepRelative + "/deep.mp3") {
                assert(!entry.isDirectory && entry.size == 2048);
            }
        }
        removeTreeDeepestFirst(root);
        std::cout << "case 3 (walkStickTree reaches a file past MAX_PATH, with nothing skipped) OK\n";
    }

    // ---- directoryTreeSizeBytes counts what is really there ----
    {
        fs::path root = freshRoot("size");
        writeThrough(root / "shallow.bin", std::string(100, 's'));
        fs::path deep = makeDeepTree(root);
        writeThrough(deep / "deep.bin", std::string(2000, 'd'));
        assert((deep / "deep.bin").native().size() > 260);

        // An exact total, not a lower bound. fs::recursive_directory_iterator
        // gets this wrong in both directions at once on Windows: it misses
        // deep.bin, and where it wanders into the working directory it can
        // add sizes of files that are not in this tree at all. Only an
        // exact figure distinguishes "counted the right files" from
        // "happened to reach a plausible number".
        assert(directoryTreeSizeBytes(root) == 2100);

        // A single file answers with its own size, and a path that is not
        // there answers zero rather than (uintmax_t)-1, which is what
        // fs::file_size reports on failure and what the old caller added
        // straight into its running total.
        assert(directoryTreeSizeBytes(deep / "deep.bin") == 2000);
        assert(directoryTreeSizeBytes(root / "does-not-exist") == 0);

        removeTreeDeepestFirst(root);
        std::cout << "case 4 (directoryTreeSizeBytes totals a tree past MAX_PATH exactly) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
