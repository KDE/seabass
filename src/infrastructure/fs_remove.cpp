// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/fs_remove.hpp"

#include "application/path_key.hpp"
#include "infrastructure/long_paths.hpp"

#include <system_error>

namespace seabass::infrastructure
{

namespace fs = std::filesystem;

namespace
{

// The path's own bytes, and back. Spelled out here rather than taken
// from stick_tree_walker.hpp, so that a target needing nothing but a
// safe remove does not have to link a tree walk as well.
std::string toUtf8(const fs::path &path)
{
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char *>(u8.data()), u8.size());
}

fs::path fromUtf8(const std::string &utf8)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
}

// Absent only when the filesystem says so. symlink_status() reports
// every failure that is not "not found" -- EIO on a dying stick, a
// Windows sharing violation -- as an unknown status, and fs::exists()
// answers false for that, which reads as "it is gone". A caller that
// believes it tells someone their track was deleted while it is still
// on the stick, which is the exact failure this file exists to stop.
bool stillThere(const fs::path &prefixed)
{
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(prefixed, ec);
    return !fs::status_known(status) || fs::exists(status);
}

}  // namespace

bool removeEntry(const fs::path &target, std::string &failure)
{
    const fs::path prefixed = longPathSafe(target);
    std::error_code ec;
    if (fs::remove(prefixed, ec)) {
        return true;  // it unlinked something; nothing to ask
    }
    // False and an error is a real failure -- a non-empty directory, a
    // read-only parent, a dying stick -- and no other spelling of the
    // name will help. Only false with NO error is the ambiguous answer
    // remove() gives to both "it was already gone" and "this filesystem
    // will not resolve that name for unlink", and those have to be told
    // apart by looking.
    if (!ec) {
        if (!stillThere(prefixed)) {
            return true;
        }
        const fs::path composed = fromUtf8(application::composedPathSpelling(toUtf8(target)));
        std::error_code linkEc;
        if (composed != target && !fs::is_symlink(fs::symlink_status(longPathSafe(composed), linkEc))) {
            // equivalent() confirms one inode, not one directory entry,
            // and it resolves symlinks -- hence the check above, so that
            // a link pointing at the same file is never the thing that
            // gets deleted.
            std::error_code sameEc;
            if (fs::equivalent(prefixed, longPathSafe(composed), sameEc) && !sameEc) {
                std::error_code composedEc;
                if (fs::remove(longPathSafe(composed), composedEc)) {
                    return true;
                }
                if (composedEc) {
                    ec = composedEc;
                }
            }
        }
    }
    // fs::remove returns false both for a real failure and for "there was
    // nothing to remove", and only the first sets ec. Passing ec.message()
    // through regardless once put "The operation completed successfully" in
    // front of a person as the reason their file could not be deleted.
    failure = ec ? ec.message() : "the filesystem removed nothing and reported no error";
    return false;
}

}  // namespace seabass::infrastructure
