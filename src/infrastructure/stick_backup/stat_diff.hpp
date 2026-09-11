// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "infrastructure/stick_backup/backup_manifest.hpp"
#include "infrastructure/stick_backup/stick_tree_walker.hpp"

namespace seabass::infrastructure::stick_backup
{

// Result of comparing the stick's current tree with the previous
// backup's manifest. Pointers refer into the TreeWalk handed in.
struct DiffResult
{
    std::vector<const TreeEntry *> unchanged;  // carry the previous entry forward
    std::vector<const TreeEntry *> added;
    std::vector<const TreeEntry *> changed;    // files only; size or mtime moved
    std::vector<std::string> removed;          // manifest paths no longer on the stick
    std::uint64_t bytesToRead = 0;             // added + changed file bytes
    // A stick whose FAT timestamps all moved by the same whole number of
    // quarter hours was carried across a timezone/DST change, not
    // rewritten; those files count as unchanged and the shift is
    // reported here (0 = none detected).
    std::int64_t uniformShiftSeconds = 0;
    std::size_t uniformlyShiftedFiles = 0;
};

// rsync's default heuristic, deliberately: a file is unchanged when its
// size matches and its mtime is within MtimeWindowSeconds of the recorded
// one (FAT/exFAT stamps have 2 s resolution). Directories are unchanged
// whenever they still exist -- their mtime moves every time a child
// changes and carries no information a restore needs.
constexpr std::int64_t MtimeWindowSeconds = 2;

// The key two paths are compared under. v1: the exact bytes. The seam
// exists so NFC/NFD folding can be added without touching the diff (see
// docs/stick-backup-plan.md, "Optimization pass").
std::string pathCompareKey(std::string_view relativePath);

DiffResult diffTreeAgainstManifest(const TreeWalk &tree, const BackupManifest *previous);

}  // namespace seabass::infrastructure::stick_backup
