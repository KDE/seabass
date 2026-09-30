// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/file_placement.hpp"

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
#include <sys/stat.h>
#endif

namespace seabass::infrastructure
{

namespace fs = std::filesystem;

#if defined(_WIN32)

std::optional<std::uint64_t> volumeIdOf(const fs::path &path)
{
    HANDLE h = CreateFileW(longPathSafe(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(h, &info) != 0;
    CloseHandle(h);
    if (!ok) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(info.dwVolumeSerialNumber);
}

#else

std::optional<std::uint64_t> volumeIdOf(const fs::path &path)
{
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {  // narrow-ok: POSIX, path::c_str() is the native UTF-8 char* here
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(st.st_dev);
}

#endif

}  // namespace seabass::infrastructure
