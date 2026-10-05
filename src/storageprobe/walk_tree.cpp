// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "walk_tree.hpp"
#include "utf8_path.hpp"

#include <filesystem>
#include <system_error>
#include <vector>

namespace storageprobe
{

namespace fs = std::filesystem;

namespace
{

// The depth below root a slice sits at; see countWalkSlices().
constexpr int kSliceDepth = 2;

}  // namespace

std::uint64_t countWalkSlices(const std::string &root, const std::function<bool(const std::string &)> &shouldDescend)
{
    // Breadth first, down to the slices' own level and not into them: the
    // same descend rule as walkTree(), so the count is what it enters.
    struct Level
    {
        fs::path path;
        std::string relative;
    };
    std::vector<Level> current{{pathFromUtf8(root), std::string()}};
    for (int depth = 1; depth <= kSliceDepth; ++depth) {
        std::vector<Level> next;
        for (const Level &level : current) {
            std::error_code ec;
            fs::directory_iterator it(level.path, ec);
            if (ec) {
                continue;
            }
            for (const fs::directory_iterator end; it != end; it.increment(ec)) {
                if (ec) {
                    break;
                }
                std::error_code entryEc;
                if (!it->is_directory(entryEc) || entryEc) {
                    continue;
                }
                const std::string name = utf8FromPath(it->path().filename());
                const std::string relative = level.relative.empty() ? name : level.relative + "/" + name;
                if (!shouldDescend || shouldDescend(relative)) {
                    next.push_back({it->path(), relative});
                }
            }
        }
        current = std::move(next);
    }
    return current.size();
}

TreeWalk walkTree(const std::string &root, const std::function<bool(const std::string &)> &shouldDescend,
                   const std::function<void(std::uint64_t)> &onProgress,
                   const std::function<void(std::uint64_t)> &onSlice)
{
    TreeWalk walk;

    struct Level
    {
        fs::path path;
        std::string relative;
        int depth = 0;
    };

    std::vector<Level> stack{{pathFromUtf8(root), std::string(), 0}};
    std::uint64_t slicesEntered = 0;

    while (!stack.empty()) {
        const Level level = stack.back();
        stack.pop_back();
        if (level.depth == kSliceDepth) {
            // Last in, first out: every folder pushed after this one, the
            // previous slice's whole subtree among them, is already done.
            if (onSlice) {
                onSlice(slicesEntered);
            }
            ++slicesEntered;
        }

        std::error_code ec;
        // Deliberately WITHOUT skip_permission_denied. That option makes an
        // EACCES directory come back empty and error-free, which is exactly
        // the silence this walker exists to remove: a directory nobody could
        // read must be reported, not quietly treated as having no files. The
        // skipping is done here instead, for every error, EACCES and macOS's
        // EPERM alike.
        fs::directory_iterator it(level.path, ec);
        if (ec) {
            // The whole point: one unreadable directory costs that
            // directory, not the stick.
            walk.skipped.push_back(level.relative.empty() ? utf8FromPath(level.path) : level.relative);
            continue;
        }

        const fs::directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                walk.skipped.push_back(level.relative.empty() ? utf8FromPath(level.path) : level.relative);
                break;
            }

            std::error_code entryEc;
            const fs::path path = it->path();
            const std::string relative =
                level.relative.empty() ? utf8FromPath(path.filename()) : level.relative + "/" + utf8FromPath(path.filename());

            if (it->is_directory(entryEc) && !entryEc) {
                if (!shouldDescend || shouldDescend(relative)) {
                    ++walk.folders;
                    stack.push_back({path, relative, level.depth + 1});
                }
                continue;
            }
            if (entryEc) {
                walk.skipped.push_back(relative);
                continue;
            }
            if (!it->is_regular_file(entryEc) || entryEc) {
                continue;  // a socket, a fifo, a symlink to nowhere: not media to read
            }

            const auto size = it->file_size(entryEc);
            if (entryEc) {
                walk.skipped.push_back(relative);
                continue;
            }
            walk.files.push_back({utf8FromPath(path), size});
            if (onProgress && (walk.files.size() & 0xFF) == 0) {
                onProgress(walk.files.size());
            }
        }
    }

    if (onSlice) {
        onSlice(slicesEntered);
    }
    return walk;
}

}  // namespace storageprobe
