// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/no_replace_rename.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
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
#include <sys/stat.h>
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
using Way = NoReplaceRename::Way;

std::atomic<NoReplaceRenameWays> g_ways{NoReplaceRenameWays::All};

std::mutex &callsMutex()
{
    static std::mutex m;
    return m;
}

NoReplaceRenameCalls &callsForTests()
{
    static NoReplaceRenameCalls calls;
    return calls;
}

NoReplaceRename answer(Status status, Way way, std::string error = {})
{
    NoReplaceRename result;
    result.status = status;
    result.way = way;
    result.error = std::move(error);
    return result;
}

// An error in words for a UTF-8 message. On Windows the number only:
// std::system_category().message() is in the system's ANSI code page.
std::string errorText(int code, const std::error_category &category)
{
#if defined(_WIN32)
    if (category == std::system_category()) {
        return "Windows error " + std::to_string(code);
    }
#endif
    return category.message(code);
}

// The last way: look, then rename. The gap between the two is documented
// in the header.
NoReplaceRename checkThenRename(const fs::path &from, const fs::path &to)
{
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(to, ec);
    if (status.type() != fs::file_type::not_found) {
        if (ec || status.type() == fs::file_type::none) {
            return answer(Status::Failed, Way::CheckThenRename, "the place of the new name could not be looked at");
        }
        return answer(Status::TargetExists, Way::CheckThenRename);
    }
    ec.clear();
    fs::rename(from, to, ec);
    if (ec) {
        return answer(Status::Failed, Way::CheckThenRename, errorText(ec.value(), ec.category()));
    }
    return answer(Status::Renamed, Way::CheckThenRename);
}

}  // namespace

void setNoReplaceRenameWaysForTests(NoReplaceRenameWays ways)
{
    g_ways = ways;
}

void setNoReplaceRenameCallsForTests(NoReplaceRenameCalls calls)
{
    const std::lock_guard<std::mutex> lock(callsMutex());
    callsForTests() = std::move(calls);
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
        return answer(Status::Renamed, Way::Atomic);
    }
    const DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
        return answer(Status::TargetExists, Way::Atomic);
    }
    return answer(Status::Failed, Way::Atomic, errorText(static_cast<int>(error), std::system_category()));
}

#else

namespace
{

// Whether an error of the atomic rename or of link() means the filesystem
// or the kernel does not offer it, rather than that it failed. EPERM is
// among them: a seccomp profile (a container, a Flatpak sandbox) answers a
// call it does not know with EPERM. Where EPERM is a real refusal, the
// fallback is refused as well and says so.
bool notSupported(int error)
{
    return error == EINVAL || error == ENOSYS || error == ENOTSUP || error == EOPNOTSUPP || error == EPERM;
}

// -1 with errno set when the call failed, or when this platform or build
// has no such call (errno ENOSYS).
int atomicRename(const char *from, const char *to)
{
#if defined(__linux__) && defined(SYS_renameat2)
    constexpr unsigned int NoReplace = 1U;  // RENAME_NOREPLACE
    return static_cast<int>(::syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, NoReplace));
#elif defined(__APPLE__)
    return ::renamex_np(from, to, RENAME_EXCL);
#else
    (void)from;
    (void)to;
    errno = ENOSYS;
    return -1;
#endif
}

// Whether `a` and `b` are one file: the same device and inode, links not
// followed.
bool sameInode(const char *a, const char *b)
{
    struct stat sa{};
    struct stat sb{};
    return ::lstat(a, &sa) == 0 && ::lstat(b, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

}  // namespace

NoReplaceRename renameNoReplace(const fs::path &from, const fs::path &to)
{
    NoReplaceRenameCalls calls;
    {
        const std::lock_guard<std::mutex> lock(callsMutex());
        calls = callsForTests();
    }
    const char *f = from.c_str();
    const char *t = to.c_str();
    const NoReplaceRenameWays ways = g_ways.load();
    if (ways == NoReplaceRenameWays::All) {
        if ((calls.atomicRename ? calls.atomicRename(f, t) : atomicRename(f, t)) == 0) {
            return answer(Status::Renamed, Way::Atomic);
        }
        const int error = errno;
        if (error == EEXIST) {
            return answer(Status::TargetExists, Way::Atomic);
        }
        if (!notSupported(error)) {
            return answer(Status::Failed, Way::Atomic, std::strerror(error));
        }
    }
    if (ways != NoReplaceRenameWays::CheckThenRenameOnly) {
        // link() refuses an existing `to` with EEXIST, atomically.
        bool linked = (calls.link ? calls.link(f, t) : ::link(f, t)) == 0;
        const int linkError = linked ? 0 : errno;
        if (!linked && linkError == EEXIST && sameInode(f, t)) {
            // `to` is `from` itself: the link was made, and the answer to
            // it was lost and the call sent again (NFS).
            linked = true;
        }
        if (linked) {
            if ((calls.unlink ? calls.unlink(f) : ::unlink(f)) != 0) {
                // The new name holds the file, which is what was asked
                // for; the old name is left as a second name for it.
                NoReplaceRename result = answer(Status::Renamed, Way::HardLink, std::strerror(errno));
                result.oldNameRemains = true;
                return result;
            }
            return answer(Status::Renamed, Way::HardLink);
        }
        if (linkError == EEXIST) {
            return answer(Status::TargetExists, Way::HardLink);
        }
        if (!notSupported(linkError)) {
            return answer(Status::Failed, Way::HardLink, std::strerror(linkError));
        }
    }
    return checkThenRename(from, to);
}

#endif

}  // namespace seabass::infrastructure
