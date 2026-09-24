// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "application/ports/cancellation_token.hpp"

namespace seabass::infrastructure::stick_backup
{

struct TreeEntry
{
    std::string relativePath;  // forward slashes, no leading or trailing slash
    bool isDirectory = false;
    std::uint64_t size = 0;
    std::int64_t mtimeUnix = 0;
};

struct TreeWalk
{
    std::vector<TreeEntry> entries;    // sorted by relativePath, so a parent precedes its children
    std::vector<std::string> skipped;  // "<path>: <reason>" -- symlinks, unreadable entries
    std::uint64_t totalFileBytes = 0;
    bool cancelled = false;
};

// Names that are never part of a stick's library and are often not even
// readable: OS metadata directories at the root, SQLite's regenerable
// -shm files anywhere, and Seabass's own write lock.
bool isExcludedFromBackup(std::string_view relativePath, bool isDirectory);

// One stat-only pass over the stick: what is there, how big, when last
// written. Never opens a file. Symlinks are skipped and reported (a
// backup stores files, never links); errors on individual entries are
// reported and skipped rather than aborting the walk (a stick pulled
// mid-walk or a permission-denied entry should not take the whole run
// down). Checks `cancel` periodically.
TreeWalk walkStickTree(const std::filesystem::path &root, application::CancellationToken cancel);

// Unix seconds from a std::filesystem timestamp, truncated toward zero.
std::int64_t toUnixSeconds(std::filesystem::file_time_type time);
std::filesystem::file_time_type fromUnixSeconds(std::int64_t seconds);

// Archive names are UTF-8 with forward slashes on every platform. These
// are the names this namespace has always used for what is now
// seabass::pathToGenericUtf8 and seabass::pathFromUtf8
// (src/infrastructure/paths/utf8_path.hpp), kept so an archive key is
// still spelled the same everywhere it is built. pathToUtf8 here is the
// generic form on purpose: its callers build archive entries and
// manifest keys, never something an OS call takes back. A translation
// unit that also sees the seabass:: ones through a using-directive has
// to qualify, or use pathToGenericUtf8.
std::string pathToUtf8(const std::filesystem::path &path);
std::filesystem::path pathFromUtf8(std::string_view utf8);

}  // namespace seabass::infrastructure::stick_backup
