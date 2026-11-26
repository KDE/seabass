// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <string>

namespace seabass::infrastructure
{

// What renameNoReplace() did.
struct NoReplaceRename
{
    enum class Status
    {
        Renamed,       // `to` is the file that was `from`; `from` is gone
        TargetExists,  // something is at `to`; nothing was touched
        Failed,        // nothing was renamed; `error` says why
    };
    Status status = Status::Failed;
    // Why not, when Failed. When Renamed through a hard link whose old
    // name could not be removed afterwards, says so: both names then hold
    // the file.
    std::string error;
    // How it was done: the filesystem refused the atomic ways, and the
    // rename went through a look at `to` and a plain rename (see below).
    bool checkedThenRenamed = false;
};

// Gives the file at `from` the name `to`, and never replaces what is at
// `to`. std::filesystem::rename replaces a file at `to` on every platform;
// this refuses instead (TargetExists), whether that file was there before
// the call or appeared a moment before the rename.
//
// The rename is atomic where the platform has a call for it:
//
//   - Linux: renameat2() with RENAME_NOREPLACE. Where the filesystem or the
//     kernel does not take the flag (EINVAL, ENOSYS, ENOTSUP), a hard link
//     to `to` and then the removal of `from`: link() fails with EEXIST when
//     `to` is taken, atomically.
//   - macOS: renamex_np() with RENAME_EXCL, the same link fallback where
//     the filesystem answers ENOTSUP.
//   - Windows: MoveFileExW without MOVEFILE_REPLACE_EXISTING, which fails
//     when `to` exists on every filesystem Windows writes.
//
// Where neither the flag nor a hard link is supported (FAT and exFAT
// through a FUSE driver or an old kernel, the msdos driver on macOS),
// `to` is looked at and then renamed to. Between the look and the
// rename is a gap: a file that appears at `to` within it is replaced.
// checkedThenRenamed says when that was the way.
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

}  // namespace seabass::infrastructure
