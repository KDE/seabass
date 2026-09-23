// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// A minimal Engine m.db: one Information row holding the rekordbox import
// counter, which is all readRekordboxImportState() asks of it -- or the
// artwork tables, which are all repairArtwork() asks of it. Plain
// sqlite3 in its own translation unit, because a test that also includes
// the edit changes pulls in sqlcipher_dyn.hpp, which declares its own
// SQLITE_* names and cannot share a unit with <sqlite3.h>.
namespace seabass::testing
{
void createEngineInformation(const std::filesystem::path &databaseFile, std::int64_t counter, int rowId = 1);
std::int64_t readEngineImportCounter(const std::filesystem::path &databaseFile);
// The two tables repairArtwork() writes, AlbumArt and Track, with one
// Track row per id in `trackIds` and no art for any of them.
void createEngineArtworkTables(const std::filesystem::path &databaseFile, const std::vector<std::int64_t> &trackIds);
// The AlbumArt hash the track with this id points at, or "" for none.
std::string engineTrackArtworkHash(const std::filesystem::path &databaseFile, std::int64_t trackId);
}  // namespace seabass::testing
