// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>

namespace seabass::infrastructure::stick_backup
{

// When the library on a stick was last written: the newest mtime among
// its catalog databases -- rekordbox's export.pdb, Engine's m.db and its
// write-ahead log, and hm.db. Audio files are not consulted: adding a
// track also rewrites the catalog, and the catalog is what an update
// between two copies of a library is about. 0 when none of them exist
// or can be stat'ed. Unix seconds; FAT keeps them at 2 s resolution.
std::int64_t libraryCatalogModifiedAt(const std::filesystem::path &stickRoot);

}  // namespace seabass::infrastructure::stick_backup
