// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace seabass::infrastructure::local
{

// The file OpenStickBackup writes next to an extracted catalog to say
// "the analysis files for this library are still in that archive".
// Two lines, UTF-8: the absolute archive path, then the canonical path of
// the cache directory the marker was written for.
inline constexpr const char *BrowsedBackupMarkerName = ".seabass-backup-source";

// Whether `libraryRoot` (the directory holding PIONEER / Engine Library)
// is the cache of a stick backup being browsed -- the one question every
// guard in the app asks, answered in one place.
//
// The marker validates itself: it is honoured only when its second line
// names this very directory. That is what stops it turning a real stick
// read-only -- it is an ordinary dotfile, so a backup taken from a cache
// would carry it, a restore would write it onto the next stick, and a
// drag-copy of the cache would bring it along; in every one of those
// places it names somewhere else, and is ignored. And it is what keeps
// the answer independent of any setting: an earlier rule keyed on the
// browse cache's *location*, which the user can change in Preferences,
// silently turning a browsed backup writable again.
bool isBrowsedBackupRoot(const std::filesystem::path &libraryRoot);

// The archive a browsed-backup root came from, or nullopt when
// isBrowsedBackupRoot() is false.
std::optional<std::filesystem::path> browsedBackupArchive(const std::filesystem::path &libraryRoot);

// The one sentence every refusal of a write to a browsed backup uses --
// the stick list, the edit session, BackupStick and CloneStick all say
// the same thing, so a person meets one wording however they arrived.
std::string browsedBackupRefusal(const std::string &label);

// weakly_canonical, or absolute when the path cannot be resolved. The
// rule that decides what "the" path of an archive is -- it keys the
// cache directory's name and the marker's first line, so it lives once.
std::filesystem::path canonicalOrAbsolute(const std::filesystem::path &path);

// Writes the marker for `cacheRoot` into `markerDir` -- normally the
// staging directory that is about to be renamed to cacheRoot, which is
// why the two are separate arguments. False if it could not be written.
bool writeBrowsedBackupMarker(const std::filesystem::path &markerDir, const std::filesystem::path &archivePath,
                              const std::filesystem::path &cacheRoot);

}  // namespace seabass::infrastructure::local
