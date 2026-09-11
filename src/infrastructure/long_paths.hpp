// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <system_error>

namespace seabass::infrastructure
{

// On Windows, paths longer than the classic MAX_PATH (260) need the \\?\
// prefix to be reached at all, and long artist/album/title paths get
// there easily. Identity elsewhere.
//
// The prefix has to be applied per call, not once at the top of an
// operation: it does not survive a path being taken apart and rebuilt,
// and a std::filesystem call handed the unprefixed path does not fail
// loudly. fs::remove() past MAX_PATH returns false with its error_code
// set to *success*, which is indistinguishable from "it was already
// gone" -- see RestoreStickBackup::writeEntry for what that cost.
inline std::filesystem::path longPathSafe(const std::filesystem::path &absolute)
{
#if defined(_WIN32)
    std::wstring native = absolute.native();
    if (native.size() > 240 && native.rfind(L"\\\\?\\", 0) != 0) {
        return std::filesystem::path(L"\\\\?\\" + std::filesystem::path(native).lexically_normal().native());
    }
#endif
    return absolute;
}

// Lists one directory, one entry at a time, in a way that survives the
// \\?\ prefix.
//
// std::filesystem::directory_iterator cannot be used for this on Windows.
// Handed a \\?\ path it does not fail -- it silently enumerates the
// process's current working directory instead, reporting success. Every
// name it then yields is a real file that is not in the directory that
// was asked about, joined onto the path that was asked about, so the
// results look plausible and are entirely wrong. Verified side by side:
// FindFirstFileW on exactly the same \\?\ pattern returns the directory's
// two real files, while directory_iterator returns 155 entries from the
// build tree the test happened to be run from.
//
// That single defect is what made two separate bugs possible: the stick
// walker could not descend past MAX_PATH (so backups silently omitted
// whole subtrees while still reporting Complete), and fs::remove_all
// spins forever on such a tree instead of reporting that it is stuck.
class DirectoryReader
{
public:
    // Opens `directory` (prefixed internally when it needs it). On
    // failure `ec` is set and next() reports the directory as empty.
    DirectoryReader(const std::filesystem::path &directory, std::error_code &ec);
    ~DirectoryReader();
    DirectoryReader(const DirectoryReader &) = delete;
    DirectoryReader &operator=(const DirectoryReader &) = delete;

    // Writes the next child's full path into `child` and returns true, or
    // returns false when the directory is exhausted or unreadable. "." and
    // ".." are never reported.
    bool next(std::filesystem::path &child, std::error_code &ec);

private:
    struct State;
    std::unique_ptr<State> m_state;
};

// Deletes `path` and everything under it, deepest first, reaching every
// entry through longPathSafe() and listing through DirectoryReader.
// Returns how many entries were removed.
//
// Use this instead of std::filesystem::remove_all for any tree that could
// contain a path past MAX_PATH. remove_all never returns on one: it
// retries the entry it cannot reach rather than giving up, so it spins at
// 100% CPU forever -- through the error_code overload, and also when its
// own root argument is already prefixed, because directory_iterator
// wanders off to the working directory as described above.
//
// Directory symlinks are removed, never descended into, which is the rule
// remove_all follows and the reason this cannot simply delete everything
// it manages to list.
std::uintmax_t removeTreeDeepestFirst(const std::filesystem::path &path);

// Total size of every regular file at or under `path`, listing through
// DirectoryReader for the same reason removeTreeDeepestFirst does:
// fs::recursive_directory_iterator cannot descend past MAX_PATH, and on
// Windows it does not even fail there -- it reports success while
// enumerating the working directory, so a total built from it can count
// files that are not in the tree at all and miss the ones that are.
//
// Unreadable entries contribute zero rather than propagating an error:
// this is a figure to show a person, and one unreadable file should not
// discard the rest. Note that fs::file_size reports failure as
// (uintmax_t)-1, so a caller adding it up without checking turns a single
// bad file into a nonsense total.
//
// Symlinks are neither counted nor followed: a link's target can sit
// outside the tree being measured.
std::uintmax_t directoryTreeSizeBytes(const std::filesystem::path &path);

}  // namespace seabass::infrastructure
