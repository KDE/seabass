// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/paths/read_access.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace seabass::infrastructure
{

ReadAccess probeReadAccess(const std::filesystem::path &file)
{
#if defined(_WIN32)
    const HANDLE handle = ::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(handle);
        return ReadAccess::Readable;
    }
    switch (::GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return ReadAccess::Missing;
    case ERROR_ACCESS_DENIED:
        return ReadAccess::Denied;
    default:
        return ReadAccess::Unreadable;
    }
#else
    const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ::close(fd);
        return ReadAccess::Readable;
    }
    switch (errno) {
    case ENOENT:
    case ENOTDIR:
        return ReadAccess::Missing;
    case EACCES:
    case EPERM:
        return ReadAccess::Denied;
    default:
        return ReadAccess::Unreadable;
    }
#endif
}

}  // namespace seabass::infrastructure
