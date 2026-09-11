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
// KNOWN GAP: Unicode normalisation. "é" as one code point and "e" plus a
// combining acute are the same filename and get different keys; a stick
// written on macOS carries the decomposed form. Fixing it needs a
// normalisation table rather than a case rule, and a half-built one
// would be worse than none. tests/path_key_test.cpp states this as an
// expectation so it is visible rather than forgotten.
std::string normalizedPathKey(const std::string &path);

}  // namespace seabass::application
