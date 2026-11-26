// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// renameNoReplace() gives a file a new name and never replaces what is
// there. Each of its three ways is run here, the fallbacks reached
// through setNoReplaceRenameWaysForTests() as a filesystem without the
// atomic call (FAT and exFAT through FUSE, an old kernel) would reach
// them:
//
//   - a free name: renamed, the old name gone, the bytes the same, and
//     the answer says which way was taken;
//   - a file at the new name: TargetExists, both files as they were;
//   - a folder at the new name: TargetExists, the folder as it was;
//   - a name outside ASCII.
//
// And, on Linux and macOS:
//
//   - the atomic way is the one taken on the test's own filesystem; a
//     filesystem without it is a failure that names the filesystem, never
//     a quiet pass through a fallback;
//   - EPERM from the atomic call (a seccomp profile) falls back to the
//     hard link, which renames;
//   - link() answering EEXIST for a link it made (an NFS retransmit):
//     renamed, the old name gone;
//   - link() made, unlink() of the old name refused: Renamed with
//     oldNameRemains, both names the one file;
//   - a new name too long for the filesystem: Failed, saying so.

#include "infrastructure/no_replace_rename.hpp"

#include "scratch_path.hpp"

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#if defined(__linux__)
#include <sys/vfs.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <sys/mount.h>
#include <sys/param.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using seabass::infrastructure::NoReplaceRename;
using seabass::infrastructure::NoReplaceRenameCalls;
using seabass::infrastructure::NoReplaceRenameWays;
using seabass::infrastructure::setNoReplaceRenameCallsForTests;
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

const char *wayName(NoReplaceRename::Way way)
{
    switch (way) {
        case NoReplaceRename::Way::Atomic:
            return "atomic";
        case NoReplaceRename::Way::HardLink:
            return "hard-link";
        case NoReplaceRename::Way::CheckThenRename:
            return "check-then-rename";
    }
    return "?";
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
        // The label is the way expected.
        check(wayName(result.way) == label, label + ": says the way taken: " + wayName(result.way));
        check(!result.oldNameRemains, label + ": no second name is left");
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

#if !defined(_WIN32)

// The filesystem `path` is on, by name where it is a common one.
std::string filesystemOf(const fs::path &path)
{
#if defined(__linux__)
    struct statfs info{};
    if (::statfs(path.c_str(), &info) != 0) {
        return "a filesystem that would not say what it is";
    }
    switch (static_cast<unsigned long>(info.f_type)) {
        case 0xEF53UL:
            return "ext2/3/4";
        case 0x9123683EUL:
            return "btrfs";
        case 0x58465342UL:
            return "XFS";
        case 0x01021994UL:
            return "tmpfs";
        case 0x2FC12FC1UL:
            return "ZFS";
        case 0x794C7630UL:
            return "overlayfs";
        case 0x6969UL:
            return "NFS";
        case 0x65735546UL:
            return "a FUSE filesystem";
        case 0x4D44UL:
            return "FAT";
        case 0x2011BAB0UL:
            return "exFAT";
        default:
            break;
    }
    char hex[32];
    std::snprintf(hex, sizeof hex, "0x%lX", static_cast<unsigned long>(info.f_type));
    return std::string("the filesystem of type ") + hex;
#else
    struct statfs info{};
    if (::statfs(path.c_str(), &info) != 0) {
        return "a filesystem that would not say what it is";
    }
    return info.f_fstypename;
#endif
}

bool sameInode(const fs::path &a, const fs::path &b)
{
    std::error_code ec;
    return fs::equivalent(a, b, ec) && !ec;
}

// The ways the system calls are answered, through the seam.
void seams(const fs::path &root)
{
    const fs::path dir = root / "seams";
    fs::create_directories(dir);
    setNoReplaceRenameWaysForTests(NoReplaceRenameWays::All);

    // The atomic way is the one taken here.
    {
        const fs::path from = dir / "atomic-from";
        const fs::path to = dir / "atomic-to";
        writeFile(from, "x");
        const NoReplaceRename result = renameNoReplace(from, to);
        check(result.status == NoReplaceRename::Status::Renamed && result.way == NoReplaceRename::Way::Atomic,
              "the atomic way is taken on the test's filesystem; " + filesystemOf(dir)
                  + " answered by the way " + wayName(result.way) + " (" + result.error + ")");
    }
    // EPERM from the atomic call, as a seccomp profile answers it.
    {
        const fs::path from = dir / "eperm-from";
        const fs::path to = dir / "eperm-to";
        writeFile(from, "sandboxed");
        setNoReplaceRenameCallsForTests({[](const char *, const char *) {
                                             errno = EPERM;
                                             return -1;
                                         },
                                         {},
                                         {}});
        const NoReplaceRename result = renameNoReplace(from, to);
        setNoReplaceRenameCallsForTests({});
        check(result.status == NoReplaceRename::Status::Renamed && result.way == NoReplaceRename::Way::HardLink,
              std::string("EPERM from the atomic call: renamed through the hard link, way ") + wayName(result.way) + " ("
                  + result.error + ")");
        check(!fs::exists(from) && readFile(to) == "sandboxed", "EPERM from the atomic call: the file has its new name only");
    }
    // link() made the link and answered EEXIST (a retransmit).
    {
        const fs::path from = dir / "retransmit-from";
        const fs::path to = dir / "retransmit-to";
        writeFile(from, "sent twice");
        setNoReplaceRenameCallsForTests({[](const char *, const char *) {
                                             errno = EINVAL;
                                             return -1;
                                         },
                                         [](const char *f, const char *t) {
                                             if (::link(f, t) != 0) {
                                                 return -1;
                                             }
                                             errno = EEXIST;
                                             return -1;
                                         },
                                         {}});
        const NoReplaceRename result = renameNoReplace(from, to);
        setNoReplaceRenameCallsForTests({});
        check(result.status == NoReplaceRename::Status::Renamed && result.way == NoReplaceRename::Way::HardLink,
              "EEXIST for a link that was made: renamed (" + result.error + ")");
        check(!fs::exists(from) && readFile(to) == "sent twice", "EEXIST for a link that was made: the old name is gone");
    }
    // The same answer for another file at `to` is still TargetExists.
    {
        const fs::path from = dir / "eexist-from";
        const fs::path to = dir / "eexist-to";
        writeFile(from, "mine");
        writeFile(to, "theirs");
        setNoReplaceRenameCallsForTests({[](const char *, const char *) {
                                             errno = EINVAL;
                                             return -1;
                                         },
                                         {},
                                         {}});
        const NoReplaceRename result = renameNoReplace(from, to);
        setNoReplaceRenameCallsForTests({});
        check(result.status == NoReplaceRename::Status::TargetExists, "EEXIST for another file: refused");
        check(readFile(from) == "mine" && readFile(to) == "theirs", "EEXIST for another file: both as they were");
    }
    // The link made, the old name could not be removed.
    {
        const fs::path from = dir / "unlink-from";
        const fs::path to = dir / "unlink-to";
        writeFile(from, "two names");
        setNoReplaceRenameCallsForTests({[](const char *, const char *) {
                                             errno = EINVAL;
                                             return -1;
                                         },
                                         {},
                                         [](const char *) {
                                             errno = EBUSY;
                                             return -1;
                                         }});
        const NoReplaceRename result = renameNoReplace(from, to);
        setNoReplaceRenameCallsForTests({});
        check(result.status == NoReplaceRename::Status::Renamed && result.oldNameRemains
                  && result.way == NoReplaceRename::Way::HardLink && !result.error.empty(),
              "unlink refused: Renamed, the old name remaining, said (" + result.error + ")");
        check(readFile(to) == "two names" && sameInode(from, to), "unlink refused: both names are the one file");
    }
    // A new name too long for the filesystem.
    {
        const fs::path from = dir / "long-from";
        const fs::path to = dir / std::string(300, 'n');
        writeFile(from, "short");
        const NoReplaceRename result = renameNoReplace(from, to);
        check(result.status == NoReplaceRename::Status::Failed && result.error.find("too long") != std::string::npos,
              "a name too long: Failed, saying so (" + result.error + ")");
        check(readFile(from) == "short", "a name too long: the file is as it was");
    }
}

#endif

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
#if !defined(_WIN32)
    seams(root);
#endif
    fs::remove_all(root);
    if (failures > 0) {
        std::cout << "no_replace_rename_test: " << failures << " FAILED\n";
        return 1;
    }
    std::cout << "no_replace_rename_test: all passed\n";
    return 0;
}
