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

// A path as an archive entry name or a manifest key: UTF-8 with forward
// slashes on every platform, so a key written on Windows matches one
// read on a Mac. This is the one definition of that spelling; every
// archive entry and every manifest key goes through it. It is
// deliberately not called pathToUtf8: that name, tree-wide, means the
// native form (src/infrastructure/paths/utf8_path.hpp), and a same-named
// function that answered with slashes would hide it inside this
// namespace. The way back is seabass::pathFromUtf8, which reads either
// separator.
std::string archiveNameOf(const std::filesystem::path &path);

}  // namespace seabass::infrastructure::stick_backup
