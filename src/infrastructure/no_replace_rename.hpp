// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace seabass::infrastructure
{

// What renameNoReplace() did.
struct NoReplaceRename
{
    enum class Status
    {
        Renamed,       // `to` is the file that was `from` (see oldNameRemains)
        TargetExists,  // something is at `to`; nothing was touched
        Failed,        // nothing was renamed; `error` says why
    };
    // The way that gave the answer.
    enum class Way
    {
        Atomic,           // renameat2 with RENAME_NOREPLACE, renamex_np, MoveFileExW
        HardLink,         // link() to `to`, then unlink() of `from`
        CheckThenRename,  // a look at `to`, then a plain rename (see below)
    };
    Status status = Status::Failed;
    Way way = Way::Atomic;
    // Why not, when Failed. When oldNameRemains, why the old name could
    // not be removed.
    std::string error;
    // Renamed through a hard link whose old name could not be removed
    // afterwards: `from` and `to` are both names of the one file. The
    // caller removes `from` again or says where it is.
    bool oldNameRemains = false;
};

// Gives the file at `from` the name `to`, and never replaces what is at
// `to`. std::filesystem::rename replaces a file at `to` on every platform;
// this refuses instead (TargetExists), whether that file was there before
// the call or appeared a moment before the rename.
//
// The rename is atomic where the platform has a call for it:
//
//   - Linux: renameat2() with RENAME_NOREPLACE. Where the filesystem or the
//     kernel does not take the flag (EINVAL, ENOSYS, ENOTSUP, and EPERM,
//     which a seccomp profile answers for a call it does not know), a hard
//     link to `to` and then the removal of `from`: link() fails with
//     EEXIST when `to` is taken, atomically. A link() that answers EEXIST
//     although `to` is `from` itself (same device and inode: an NFS
//     retransmit of a link that was made) counts as made.
//   - macOS: renamex_np() with RENAME_EXCL, the same link fallback where
//     the filesystem answers ENOTSUP.
//   - Windows: MoveFileExW without MOVEFILE_REPLACE_EXISTING, which fails
//     when `to` exists on every filesystem Windows writes.
//
// Where neither the flag nor a hard link is supported (FAT and exFAT
// through a FUSE driver or an old kernel, the msdos driver on macOS),
// `to` is looked at and then renamed to. Between the look and the
// rename is a gap: a file that appears at `to` within it is replaced.
// `way` says which way it was.
//
// Both paths are passed through longPathSafe() here. Never throws.
NoReplaceRename renameNoReplace(const std::filesystem::path &from, const std::filesystem::path &to);

// For tests only: which ways renameNoReplace() may use, to reach the
// fallbacks on a filesystem that has the atomic call.
enum class NoReplaceRenameWays
{
    All,
    WithoutAtomicRename,  // as on a filesystem without RENAME_NOREPLACE
    CheckThenRenameOnly,  // as on FAT without the flag: no hard links either
};
void setNoReplaceRenameWaysForTests(NoReplaceRenameWays ways);

// For tests only, Linux and macOS: the system calls renameNoReplace()
// makes, each answering as the call does (0, or -1 with errno set). An
// empty one is the real call. {} puts all of them back.
struct NoReplaceRenameCalls
{
    std::function<int(const char *from, const char *to)> atomicRename;
    std::function<int(const char *from, const char *to)> link;
    std::function<int(const char *path)> unlink;
};
void setNoReplaceRenameCallsForTests(NoReplaceRenameCalls calls);

}  // namespace seabass::infrastructure
