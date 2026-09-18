// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace storageprobe
{

// Every regular file under root, with the directories that could not be
// opened reported rather than fatal.
//
// std::filesystem::recursive_directory_iterator cannot do this. Its
// skip_permission_denied has two halves and both are wrong here: it makes
// an EACCES directory come back silently empty, so a check reports a clean
// bill of health for a stick it never read, and it does not cover EPERM at
// all, which is what macOS answers for a directory the system protects -- .Spotlight-V100, which macOS creates on
// every USB volume it indexes. The iterator then sets the error code, and
// the usual `for (; !ec && it != end; it.increment(ec))` shape abandons the
// entire walk. Measured on a real 31.5 GB stick: the walk stopped at
// /Volumes/A1/.Spotlight-V100 having seen zero files, and the wear check
// built on it reported "Nothing was read." for a stick holding 85 tracks.
//
// It does not reproduce on a disk image, because a disk image has no
// Spotlight index -- which is why the rig never saw it.
//
// So this walks by hand, as the backup walker already does for its own
// reason (Windows and MAX_PATH), and a directory it cannot open becomes an
// entry in `skipped` while the rest of the stick is still read.
struct TreeWalk
{
    struct File
    {
        std::string path;
        std::uint64_t size = 0;
    };

    std::vector<File> files;
    // Directories descended into, pruned ones excluded -- what a caller
    // showing "N folders" means by it.
    std::uint64_t folders = 0;
    // Directories that could not be opened, with the reason. Never silent:
    // a check that reads less than the whole stick has to say so.
    std::vector<std::string> skipped;
};

// `shouldDescend` is asked before entering a directory (its path relative
// to root, generic separators); returning false prunes it, which is how
// callers apply their own exclusions. Empty means descend everywhere.
// `onProgress` is called every so often with the running file count, so a
// slow walk of a big stick can still be cancelled by the caller throwing.
TreeWalk walkTree(const std::string &root,
                   const std::function<bool(const std::string &relativePath)> &shouldDescend = {},
                   const std::function<void(std::uint64_t filesSoFar)> &onProgress = {});

}  // namespace storageprobe
