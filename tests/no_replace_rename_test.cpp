// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// renameNoReplace() gives a file a new name and never replaces what is
// there. Each of its three ways is run here, the fallbacks reached
// through setNoReplaceRenameWaysForTests() as a filesystem without the
// atomic call (FAT and exFAT through FUSE, an old kernel) would reach
// them:
//
//   - a free name: renamed, the old name gone, the bytes the same;
//   - a file at the new name: TargetExists, both files as they were;
//   - a folder at the new name: TargetExists, the folder as it was;
//   - a name outside ASCII.

#include "infrastructure/no_replace_rename.hpp"

#include "scratch_path.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using seabass::infrastructure::NoReplaceRename;
using seabass::infrastructure::NoReplaceRenameWays;
using seabass::infrastructure::renameNoReplace;
using seabass::infrastructure::setNoReplaceRenameWaysForTests;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (ok) {
        std::cout << "ok: " << what << "\n";
    } else {
        std::cout << "FAILED: " << what << "\n";
        ++failures;
    }
}

void writeFile(const fs::path &path, const std::string &content)
{
    std::ofstream out(path, std::ios::binary);
    out << content;
}

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void run(const fs::path &root, NoReplaceRenameWays ways, const std::string &label)
{
    setNoReplaceRenameWaysForTests(ways);
    const fs::path dir = root / label;
    fs::create_directories(dir);

    {
        const fs::path from = dir / "from.flac";
        const fs::path to = dir / seabass::pathFromUtf8("to \xC3\xA9\xC3\x9F.flac");
        writeFile(from, "the checked file");
        const NoReplaceRename result = renameNoReplace(from, to);
        check(result.status == NoReplaceRename::Status::Renamed, label + ": a free name is taken (" + result.error + ")");
        check(!fs::exists(from) && readFile(to) == "the checked file", label + ": the file has its new name only");
        check(result.checkedThenRenamed == (ways == NoReplaceRenameWays::CheckThenRenameOnly),
              label + ": says whether it looked and then renamed");
    }
    {
        const fs::path from = dir / "from2.flac";
        const fs::path to = dir / "taken.flac";
        writeFile(from, "new");
        writeFile(to, "someone else's file");
        const NoReplaceRename result = renameNoReplace(from, to);
        check(result.status == NoReplaceRename::Status::TargetExists, label + ": a file at the name: refused");
        check(readFile(to) == "someone else's file", label + ": the file at the name is as it was");
        check(readFile(from) == "new", label + ": the file to rename is as it was");
    }
    {
        const fs::path from = dir / "from3.flac";
        const fs::path to = dir / "folder.flac";
        writeFile(from, "new");
        fs::create_directories(to / "inside");
        const NoReplaceRename result = renameNoReplace(from, to);
        check(result.status == NoReplaceRename::Status::TargetExists, label + ": a folder at the name: refused");
        check(fs::is_directory(to / "inside") && readFile(from) == "new", label + ": both as they were");
    }
}

}  // namespace

int main()
{
    const fs::path root = seabass::testing::scratchRoot() / "no-replace-rename-test";
    fs::remove_all(root);
    fs::create_directories(root);
    run(root, NoReplaceRenameWays::All, "atomic");
    run(root, NoReplaceRenameWays::WithoutAtomicRename, "hard-link");
    run(root, NoReplaceRenameWays::CheckThenRenameOnly, "check-then-rename");
    setNoReplaceRenameWaysForTests(NoReplaceRenameWays::All);
    fs::remove_all(root);
    if (failures > 0) {
        std::cout << "no_replace_rename_test: " << failures << " FAILED\n";
        return 1;
    }
    std::cout << "no_replace_rename_test: all passed\n";
    return 0;
}
