// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/stick_backup/stick_tree_walker.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <system_error>
#include <vector>

#include "infrastructure/engine/engine_library_layout.hpp"
#include "infrastructure/local/browsed_backup_root.hpp"
#include "infrastructure/long_paths.hpp"

namespace seabass::infrastructure::stick_backup
{

namespace fs = std::filesystem;

namespace
{

constexpr std::array<std::string_view, 5> ExcludedRootDirectories = {
    "System Volume Information", "$RECYCLE.BIN", ".Trashes", ".Spotlight-V100", ".fseventsd",
};
constexpr std::string_view WriteLockPath = "Seabass/backups/.write.lock";
// The browse cache's own marker. A backup taken from one must not carry
// it: restored onto a stick it would be a stray file (the marker names
// the directory it was written for, so it would not be honoured -- but
// it need not travel at all).
constexpr std::string_view BrowsedBackupMarker = local::BrowsedBackupMarkerName;

std::string_view firstComponent(std::string_view relativePath)
{
    std::size_t slash = relativePath.find('/');
    return slash == std::string_view::npos ? relativePath : relativePath.substr(0, slash);
}

std::string_view lastComponent(std::string_view relativePath)
{
    std::size_t slash = relativePath.rfind('/');
    return slash == std::string_view::npos ? relativePath : relativePath.substr(slash + 1);
}

// One open directory in the depth-first walk, with the relative path that
// leads to it ("" at the root, otherwise "Contents/Album/").
struct WalkLevel
{
    std::unique_ptr<DirectoryReader> reader;
    std::string prefix;
};

}  // namespace

bool isExcludedFromBackup(std::string_view relativePath, bool isDirectory)
{
    std::string_view first = firstComponent(relativePath);
    for (std::string_view excluded : ExcludedRootDirectories) {
        if (first == excluded) {
            return true;
        }
    }
    if (relativePath == WriteLockPath || relativePath == BrowsedBackupMarker) {
        return true;
    }
    return !isDirectory && engine::isSqliteShmFile(lastComponent(relativePath));
}

std::int64_t toUnixSeconds(fs::file_time_type time)
{
    using namespace std::chrono;
    auto sys = clock_cast<system_clock>(time);
    return duration_cast<seconds>(sys.time_since_epoch()).count();
}

fs::file_time_type fromUnixSeconds(std::int64_t secondsSinceEpoch)
{
    using namespace std::chrono;
    system_clock::time_point sys{seconds(secondsSinceEpoch)};
    return clock_cast<fs::file_time_type::clock>(sys);
}

std::string pathToUtf8(const fs::path &path)
{
    std::u8string u8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char *>(u8.data()), u8.size());
}

fs::path pathFromUtf8(std::string_view utf8)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
}

TreeWalk walkStickTree(const fs::path &root, application::CancellationToken cancel)
{
    TreeWalk walk;
    std::error_code ec;
    // Lexical, not fs::relative(): the latter canonicalizes, which follows
    // symlinks (a link would be recorded under its target's name) and
    // costs a realpath per entry.
    fs::path rootNormalized = root.lexically_normal();
    if (rootNormalized.filename().empty()) {
        rootNormalized = rootNormalized.parent_path();
    }
    // Depth-first by hand rather than fs::recursive_directory_iterator, so
    // that every directory is opened through longPathSafe(). On Windows
    // the recursive iterator cannot descend into a directory whose path is
    // past MAX_PATH: it reports "No such file or directory" for a
    // directory that plainly exists, and everything below it is then
    // silently missing from the backup while the run still reports
    // Complete, because walk.skipped only becomes a warning and warnings
    // do not change the outcome status. A long-ish artist/album path on a
    // stick mounted at a long-ish mount point reaches that depth easily.
    //
    // Carrying the relative prefix down as we go also keeps the entry
    // names clean: lexically_relative() would otherwise have to reconcile
    // a \\?\ prefix that appears partway down the tree against a root
    // that does not have one.
    std::vector<WalkLevel> stack;
    {
        auto reader = std::make_unique<DirectoryReader>(rootNormalized, ec);
        if (ec) {
            walk.skipped.push_back(root.string() + ": " + ec.message());
            return walk;
        }
        stack.push_back({std::move(reader), std::string()});
    }

    std::size_t sinceCancelCheck = 0;
    while (!stack.empty()) {
        fs::path childPath;
        if (!stack.back().reader->next(childPath, ec)) {
            if (ec) {
                walk.skipped.push_back(stack.back().prefix + ": " + ec.message());
                ec.clear();
            }
            stack.pop_back();
            continue;
        }
        const std::string prefix = stack.back().prefix;
        if (++sinceCancelCheck >= 256) {
            sinceCancelCheck = 0;
            if (cancel.cancelled()) {
                walk.cancelled = true;
                break;
            }
        }

        // Prefixed for every query: a child of a directory that was still
        // short enough to reach unprefixed can itself be past MAX_PATH,
        // and there the unprefixed query fails and a perfectly readable
        // file gets recorded as unreadable. Single-file operations
        // (symlink_status, last_write_time, file_size) do honour the
        // prefix -- it is only directory listing that does not, which is
        // what DirectoryReader is for.
        const fs::path full = longPathSafe(childPath);
        const std::string relative = prefix + pathToUtf8(childPath.filename());

        std::error_code statusEc;
        fs::file_status linkStatus = fs::symlink_status(full, statusEc);
        if (statusEc) {
            walk.skipped.push_back(relative + ": " + statusEc.message());
            continue;
        }
        if (fs::is_symlink(linkStatus)) {
            walk.skipped.push_back(relative + ": symbolic link");
            continue;
        }
        const bool isDirectory = fs::is_directory(linkStatus);
        if (isExcludedFromBackup(relative, isDirectory)) {
            continue;
        }
        if (!isDirectory && !fs::is_regular_file(linkStatus)) {
            walk.skipped.push_back(relative + ": not a regular file");
            continue;
        }

        TreeEntry treeEntry;
        treeEntry.relativePath = relative;
        treeEntry.isDirectory = isDirectory;
        fs::file_time_type mtime = fs::last_write_time(full, statusEc);
        if (statusEc) {
            walk.skipped.push_back(relative + ": " + statusEc.message());
            continue;
        }
        treeEntry.mtimeUnix = toUnixSeconds(mtime);
        if (!isDirectory) {
            treeEntry.size = fs::file_size(full, statusEc);
            if (statusEc) {
                walk.skipped.push_back(relative + ": " + statusEc.message());
                continue;
            }
            walk.totalFileBytes += treeEntry.size;
        }
        walk.entries.push_back(std::move(treeEntry));

        if (isDirectory) {
            std::error_code openEc;
            auto child = std::make_unique<DirectoryReader>(full, openEc);
            if (openEc) {
                walk.skipped.push_back(relative + ": " + openEc.message());
                continue;
            }
            stack.push_back({std::move(child), relative + "/"});
        }
    }
    std::sort(walk.entries.begin(), walk.entries.end(),
              [](const TreeEntry &a, const TreeEntry &b) { return a.relativePath < b.relativePath; });
    return walk;
}

}  // namespace seabass::infrastructure::stick_backup
