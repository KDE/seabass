// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

namespace seabass::application
{

// A file path reduced to a key two spellings of the same physical file
// both land on.
//
// Three separate decisions rest on this comparison and all three are
// destructive if it says "different" about one file: whether a file on
// disk is referenced by any catalog (findUnreferencedFiles), whether a
// file queued for deletion is still needed (resolvePendingDeletions),
// and whether two catalog rows describe one file or two
// (collapseCatalogRows). They had two byte-identical copies of this
// between them before this header existed; a third would have been the
// moment one of them quietly drifted.
//
// Backslashes become slashes explicitly, before std::filesystem sees
// anything: it only treats '\' as a separator on Windows, so leaving it
// to the platform would make a stick written on Windows compare
// differently when read on Linux.
//
// Trailing spaces, tabs and NULs go first. export.pdb keeps its strings
// in fixed-length fields and pads them, so a path from a rekordbox
// catalog arrives padded while the same path walked off the filesystem
// does not; every one of the fixture's 1161 paths was affected, which
// meant every referenced file read as unreferenced.
//
// Then lowercased, because exFAT and NTFS are case-insensitive:
// "Contents/A/b.mp3" and "contents/a/b.mp3" are one file, and comparing
// them case-sensitively would call a referenced file unreferenced. Those
// filesystems are case-insensitive over Unicode rather than over ASCII,
// so the folding covers Latin-1, Latin Extended-A, Greek and Cyrillic --
// the alphabets a music library is actually written in. It is not a full
// Unicode table: every mapping in it is one a person can check against a
// code chart, which is the right trade for a function that decides
// whether a file gets deleted.
//
// Note which way the error runs. Normalizing too little is the dangerous
// direction: two spellings of one file get different keys, the file
// reads as unreferenced, and unreferenced files are what Seabass offers
// to delete. Normalizing too much only ever moves a file toward "still
// referenced" or two rows toward "the same file".
//
// Unicode normalisation is folded too, since 2026-09-17: "é" as one code
// point and "e" plus a combining acute are the same filename, and macOS
// stores the decomposed form on FAT and exFAT -- a stick restored there
// read every accented track as an extra, and Clean Up would have called
// the same files unreferenced. The table is generated from the Unicode
// database by tools/generate_nfc_compositions.py
// (src/application/nfc_compositions.inc); combining marks are composed in
// the order they arrive, without canonical reordering first, which leaves
// an unusually ordered pair of marks unfolded -- the safe direction.
std::string normalizedPathKey(const std::string &path);

// The same spelling with its combining marks composed, and nothing else
// done to it: no lowercasing, no "." or ".." collapsing, no trimming.
// A name to hand back to the filesystem, rather than a key to compare.
//
// macOS needs one. Its exFAT driver hands out decomposed names from a
// directory read ("Ko" + U+0308 for "Kölsch") and accepts either
// spelling for stat -- but unlink and rmdir take only the composed one
// and answer ENOENT for the other, which std::filesystem reports as
// "there was nothing to remove". A caller that has just been refused
// can retry with this, once the filesystem has confirmed the two names
// are the same entry.
std::string composedPathSpelling(const std::string &path);

// Whether two path strings name the same place, by normalizedPathKey.
//
// For every check in the app that holds one path and is handed another:
// the two can be spelled differently while naming one directory. Every
// QString path is forward-slash (src/gui/qt_path.hpp) while a mount point
// or a canonical path kept as std::string is native, so on Windows "E:\"
// meets "E:/" and "C:\Music\Set" meets "C:/Music/Set/". A plain ==
// there says "different", and round 9 of the shakedown found Close Folder
// doing nothing on Windows because of exactly that. The separators are
// folded on every platform, so this is tested on Linux with the Windows
// spellings.
//
// An empty path is the same as nothing, not as another empty path.
bool samePath(const std::string &a, const std::string &b);

// Whether `path` is `root` itself or somewhere inside it, with both sides
// through normalizedPathKey first: pathIsUnder (stick_path_match.hpp) on
// the keys, so "E:/PIONEER/rekordbox" is under "E:\" and
// "/media/RV22" is still not under "/media/RV2".
bool pathIsAtOrUnder(const std::string &path, const std::string &root);

}  // namespace seabass::application
