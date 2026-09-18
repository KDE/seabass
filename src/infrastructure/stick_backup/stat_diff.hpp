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

// Whether a file's mtime on a stick is the one that was recorded for it.
//
// The window above, plus one exception for the floor of the FAT epoch.
// FAT and exFAT count from 1980-01-01 00:00 and store *local* time with
// no timezone in it, so a driver cannot write a stamp below its own
// local floor: it clamps. macOS's exFAT driver takes 1980-01-01
// 00:00:00Z and gives back 1980-01-01 00:00 local, an hour off here and
// up to twelve elsewhere. rekordbox stamps its ANLZ files with the
// epoch itself -- a filesystem's way of saying "no date" -- so an exact
// restore wrote those two files, read them back an hour off, and called
// them still-to-write on every run, for ever.
//
// So: two stamps both within one timezone of the epoch, differing by a
// whole number of quarter hours, are the same clamped stamp. Not "both
// somewhere in 1980-01-01": a day-wide window would quietly forgive a
// real edit, and since no real file is dated there, nothing would ever
// fail to tell us.
bool mtimeMatchesRecorded(std::int64_t recordedUnix, std::int64_t onDiskUnix);

// The key two paths are compared under. v1: the exact bytes. The seam
// exists so NFC/NFD folding can be added without touching the diff (see
// docs/stick-backup-plan.md, "Optimization pass").
std::string pathCompareKey(std::string_view relativePath);

DiffResult diffTreeAgainstManifest(const TreeWalk &tree, const BackupManifest *previous);

}  // namespace seabass::infrastructure::stick_backup
