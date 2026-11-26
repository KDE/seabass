// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/no_replace_rename.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <system_error>

#include "infrastructure/long_paths.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace seabass::infrastructure
{

namespace
{

namespace fs = std::filesystem;
using Status = NoReplaceRename::Status;

std::atomic<NoReplaceRenameWays> g_ways{NoReplaceRenameWays::All};

NoReplaceRename answer(Status status, std::string error = {}, bool checked = false)
{
    NoReplaceRename result;
    result.status = status;
    result.error = std::move(error);
    result.checkedThenRenamed = checked;
    return result;
}

// The last way: look, then rename. The gap between the two is documented
// in the header.
NoReplaceRename checkThenRename(const fs::path &from, const fs::path &to)
{
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(to, ec);
    if (status.type() != fs::file_type::not_found) {
        if (ec || status.type() == fs::file_type::none) {
            return answer(Status::Failed, "the place of the new name could not be looked at", true);
        }
        return answer(Status::TargetExists, {}, true);
    }
    ec.clear();
    fs::rename(from, to, ec);
    if (ec) {
        return answer(Status::Failed, ec.message(), true);
    }
    return answer(Status::Renamed, {}, true);
}

}  // namespace

void setNoReplaceRenameWaysForTests(NoReplaceRenameWays ways)
{
    g_ways = ways;
}

#if defined(_WIN32)

NoReplaceRename renameNoReplace(const fs::path &fromPath, const fs::path &toPath)
{
    const fs::path from = longPathSafe(fromPath);
    const fs::path to = longPathSafe(toPath);
    if (g_ways.load() == NoReplaceRenameWays::CheckThenRenameOnly) {
        return checkThenRename(from, to);
    }
    // Without MOVEFILE_REPLACE_EXISTING the move fails when `to` exists.
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return answer(Status::Renamed);
    }
    const DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
        return answer(Status::TargetExists);
    }
    return answer(Status::Failed, std::system_category().message(static_cast<int>(error)));
}

#else

namespace
{

// Whether an error of the atomic rename or of link() means the filesystem
// or the kernel does not offer it, rather than that it failed.
bool notSupported(int error)
{
    return error == EINVAL || error == ENOSYS || error == ENOTSUP || error == EOPNOTSUPP || error == EPERM;
}

// -1 with errno set when the call failed; -2 when this platform or build
// has no such call.
int atomicRename(const fs::path &from, const fs::path &to)
{
#if defined(__linux__) && defined(SYS_renameat2)
    constexpr unsigned int NoReplace = 1U;  // RENAME_NOREPLACE
    return static_cast<int>(::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), NoReplace));
#elif defined(__APPLE__)
    return ::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL);
#else
    (void)from;
    (void)to;
    return -2;
#endif
}

}  // namespace

NoReplaceRename renameNoReplace(const fs::path &from, const fs::path &to)
{
    const NoReplaceRenameWays ways = g_ways.load();
    if (ways == NoReplaceRenameWays::All) {
        const int renamed = atomicRename(from, to);
        if (renamed == 0) {
            return answer(Status::Renamed);
        }
        if (renamed == -1) {
            const int error = errno;
            if (error == EEXIST) {
                return answer(Status::TargetExists);
            }
            // EPERM from rename itself is a real refusal (a sticky folder),
            // not a missing flag.
            if (error == EPERM || !notSupported(error)) {
                return answer(Status::Failed, std::strerror(error));
            }
        }
    }
    if (ways != NoReplaceRenameWays::CheckThenRenameOnly) {
        // link() refuses an existing `to` with EEXIST, atomically.
        if (::link(from.c_str(), to.c_str()) == 0) {
            if (::unlink(from.c_str()) != 0) {
                // The new name holds the file, which is what was asked
                // for; the old name is left as a second name for it.
                const int error = errno;
                return answer(Status::Renamed, std::string("the old name stays: ") + std::strerror(error));
            }
            return answer(Status::Renamed);
        }
        const int error = errno;
        if (error == EEXIST) {
            return answer(Status::TargetExists);
        }
        if (!notSupported(error)) {
            return answer(Status::Failed, std::strerror(error));
        }
    }
    return checkThenRename(from, to);
}

#endif

}  // namespace seabass::infrastructure
