// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// engine_import_probe's OneLibrary writes, in a translation unit of their
// own: the OneLibrary writer's header brings in SQLCipher's names, which
// collide with the plain <sqlite3.h> the rest of the probe reads m.db with.

#include "engine_import_probe_onelibrary.hpp"

#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"

namespace seabass::probe
{

bool hasOneLibrary(const std::string &pioneerRoot)
{
    return infrastructure::onelibrary::OneLibraryCueWriter::existsFor(pioneerRoot);
}

void writeOneLibraryCues(const std::string &pioneerRoot, const std::string &filePath,
                         const std::vector<domain::CuePoint> &cues)
{
    infrastructure::onelibrary::OneLibraryCueWriter writer(pioneerRoot);
    writer.writeCuesForPath(filePath, cues);
}

}  // namespace seabass::probe
