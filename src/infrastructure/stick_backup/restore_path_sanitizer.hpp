// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "infrastructure/long_paths.hpp"

namespace seabass::infrastructure::stick_backup
{

// longPathSafe started here, but the walker needs it too and cannot
// include this header (this one already includes the walker). It now
// lives in infrastructure/long_paths.hpp alongside removeTreeDeepestFirst;
// this keeps the unqualified name resolving for everything that reaches
// it through `using namespace ...::stick_backup`.
using infrastructure::longPathSafe;

enum class TargetOs
{
    Posix,
    Windows,
};

constexpr TargetOs hostTargetOs()
{
#if defined(_WIN32)
    return TargetOs::Windows;
#else
    return TargetOs::Posix;
#endif
}

// Turns an archive entry name into a path that is safe to create under a
// restore target, or explains why it is not. Rejects everything that
// could land outside the target or cannot exist on the target OS:
// absolute paths, drive letters, `.` / `..` segments, empty segments,
// backslashes and NUL (never legal in a FAT/exFAT/NTFS name), and on
// Windows the reserved device names (CON, AUX, COM1 ...) with any
// extension, trailing dots or spaces, and the characters <>:"|?*.
// A rejected entry is reported by the caller, never silently skipped.
//
// Trailing '/' (a directory entry) is stripped; `isDirectory` says so.
std::optional<std::filesystem::path> sanitizeEntryName(std::string_view entryName, TargetOs os, std::string *reason = nullptr,
                                                      bool *isDirectory = nullptr);

}  // namespace seabass::infrastructure::stick_backup
