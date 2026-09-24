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

TreeWalk walkTree(const std::string &root, const std::function<bool(const std::string &)> &shouldDescend,
                   const std::function<void(std::uint64_t)> &onProgress)
{
    TreeWalk walk;

    struct Level
    {
        fs::path path;
        std::string relative;
    };

    std::vector<Level> stack{{pathFromUtf8(root), std::string()}};

    while (!stack.empty()) {
        const Level level = stack.back();
        stack.pop_back();

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
                    stack.push_back({path, relative});
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

    return walk;
}

}  // namespace storageprobe
