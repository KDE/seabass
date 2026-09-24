// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <map>
#include <optional>
#include <string>

namespace seabass::testing
{

// Track id -> whether that row's Track.lastEditTime is set (non-NULL and
// positive), read straight out of an Engine library's Database2/m.db, so a
// test can hold the reader to what the database says without going
// through it. Nullopt when the database or the column cannot be read.
//
// Its own file because it needs <sqlite3.h>, and the OneLibrary headers a
// test like corpus_test also includes declare their own SQLITE_* names in
// place of it.
std::optional<std::map<std::string, bool>> engineTrackDatedById(const std::string &engineLibraryRoot);

}  // namespace seabass::testing
