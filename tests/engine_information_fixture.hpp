// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>

// A minimal Engine m.db: one Information row holding the rekordbox import
// counter, which is all readRekordboxImportState() asks of it. Plain
// sqlite3 in its own translation unit, because a test that also includes
// the edit changes pulls in sqlcipher_dyn.hpp, which declares its own
// SQLITE_* names and cannot share a unit with <sqlite3.h>.
namespace seabass::testing
{
void createEngineInformation(const std::filesystem::path &databaseFile, std::int64_t counter, int rowId = 1);
std::int64_t readEngineImportCounter(const std::filesystem::path &databaseFile);
}  // namespace seabass::testing
