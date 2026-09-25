// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// The one rule for paths as strings, on every platform:
//
//     Every std::string that holds a path is UTF-8, and it becomes a
//     std::filesystem::path, or comes from one, only through the two
//     functions below.
//
// std::filesystem::path's own std::string conversions are UTF-8 only on
// POSIX. On Windows (MSVC and MinGW alike) path(std::string) reads the
// bytes in the ANSI code page, and path::string() narrows back through
// it, throwing for any character the code page lacks. A DJ stick has
// been through macOS, Linux, rekordbox and Engine, and folder names
// outside code page 1252 are ordinary: Japanese and Cyrillic artists,
// emoji in playlist names. Round 8 of the shakedown found the benchmark
// and a restore failing on exactly that.
//
// QString::toStdString() is UTF-8 and QString::fromStdString() reads
// UTF-8, so a QString path goes pathFromUtf8(q.toStdString()) and comes
// back QString::fromStdString(pathToUtf8(p)) -- src/gui/qt_path.hpp
// wraps both. sqlite3_open() takes UTF-8 on every platform, so a
// database opens with pathToUtf8(p), never p.string(). Streams and
// Win32 calls take the path itself: std::ifstream(p), CreateFileW(p.c_str()).
//
// tests/no_narrow_path_conversions_test.cpp scans the tree for the
// conversions this replaces, so a new one fails the suite.
namespace seabass
{

inline std::filesystem::path pathFromUtf8(std::string_view utf8)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
}

// Native separators: what a file dialog, a log line or an error message
// shows, and what an OS call would take back.
inline std::string pathToUtf8(const std::filesystem::path &path)
{
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char *>(u8.data()), u8.size());
}

// Forward slashes on every platform: an archive entry, a manifest key,
// anything compared or stored across platforms.
inline std::string pathToGenericUtf8(const std::filesystem::path &path)
{
    const std::u8string u8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char *>(u8.data()), u8.size());
}

// A path as a map key or for an equality test is not this header's
// business: application::normalizedPathKey (src/application/path_key.hpp)
// folds separators, trailing separators, case and Unicode composition,
// and is the one normaliser every lookup keyed by a path goes through.

}  // namespace seabass
