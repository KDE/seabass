// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

#include "application/use_cases/plan_engine_update.hpp"

namespace seabass::infrastructure::local
{

// What Sync after Rekordbox Export needs from the stick besides the two
// catalogs' tracks, read without writing anything: export.pdb's playlist
// tree and sequence, Engine's playlists and each row's pdbImportKey, the
// baseline file, and a fileExists that stats the stick. `pioneerRoot` and
// `engineLibraryPath` are the two catalog folders of one stick; the stick
// root is their parent.
//
// Throws std::runtime_error when a catalog cannot be read, or when the
// stick has a baseline that cannot be read (named in the message): the
// plan falls back only for a stick that has none, never for one whose
// record is damaged.
application::EngineUpdateStickFacts readEngineUpdateStickFacts(const std::string &pioneerRoot,
                                                               const std::string &engineLibraryPath);

}  // namespace seabass::infrastructure::local
