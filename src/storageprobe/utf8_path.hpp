// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <string>

namespace storageprobe
{

// Every path this library takes or hands back as a std::string is UTF-8,
// on every platform.
//
// std::filesystem::path's own std::string conversions are UTF-8 only on
// POSIX. On Windows (MSVC and MinGW alike) path(std::string) reads the
// bytes in the ANSI code page and path::string() narrows back through it,
// which throws for any character the code page lacks. A stick is a FAT or
// exFAT volume that has been through Macs, Linux and rekordbox, and folder
// names outside the code page are ordinary (Japanese and Cyrillic artists,
// emoji). So nothing here goes through those conversions; everything goes
// through these two.
//
// Header-only and dependency-free, because this library is meant to be
// lifted into another tree (README.md).
inline std::filesystem::path pathFromUtf8(const std::string &utf8)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
}

inline std::string utf8FromPath(const std::filesystem::path &path)
{
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char *>(u8.data()), u8.size());
}

}  // namespace storageprobe
