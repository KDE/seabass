// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace seabass::infrastructure
{

// Every run of readable text in `file` that cannot be accounted for, as
// distinct fragments in sorted order. Empty means nothing was found.
//
// This exists because AnonymizationVerifier's other checks read values
// back through this project's own readers, and a reader returns live rows
// -- exactly the rows the anonymizer just overwrote. It can never see a
// value still legible in a SQLite freeblock, in a page the freelist has
// released, or in the slack DeviceSQL leaves when a row is removed. The
// 0.6-era fixture in this repository passed every reader check while
// holding 52 real artist names in engine/Database2/m.db and 403 in
// rekordbox/export.pdb.
//
// "Accounted for" means a fragment made entirely of placeholder text,
// words the database's own schema declares, or the seam where the byte
// scanner reads across the join between two packed values. It is a
// tripwire, not a proof: a leak made entirely of schema words would pass.
//
// Lives apart from anonymization_verifier.cpp because it needs
// <sqlite3.h>, and that cannot be included alongside sqlcipher_dyn.hpp,
// which declares its own SQLITE_* constants.
std::vector<std::string> readableTextInRawBytes(const std::filesystem::path &file);

}  // namespace seabass::infrastructure
