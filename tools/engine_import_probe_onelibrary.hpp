// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::probe
{

// Whether the stick has exportLibrary.db beside export.pdb.
bool hasOneLibrary(const std::string &pioneerRoot);

// One track's complete cue set into exportLibrary.db, through Seabass's
// own OneLibraryCueWriter. Throws as that writer does.
void writeOneLibraryCues(const std::string &pioneerRoot, const std::string &filePath,
                         const std::vector<domain::CuePoint> &cues);

}  // namespace seabass::probe
