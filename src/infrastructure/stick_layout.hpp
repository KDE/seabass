// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

namespace seabass::infrastructure
{

// Where `format`'s catalog lives on the stick that `libraryPath` belongs
// to, or empty when that catalog is not on this stick.
//
// `libraryPath` is any catalog directory on the stick (".../PIONEER",
// ".../Engine Library"); the stick root is its parent, so this answers
// "given one catalog, where are the others".
//
// Removing a duplicate *file* means removing its row from every catalog
// that lists it, and a caller holding one catalog's path needs the rest
// to do that. Deriving them at each call site would scatter the
// PIONEER/"Engine Library" layout knowledge; it lives here instead.
//
// Existence is checked rather than assumed. "rekordbox" and
// "onelibrary" share the PIONEER directory but are different files
// inside it (export.pdb, exportLibrary.db) and a stick can carry either
// without the other -- returning a path for a catalog that is not there
// would send a writer at a file it would have to create.
std::string catalogPathFor(const std::string &format, const std::string &libraryPath);

}  // namespace seabass::infrastructure
