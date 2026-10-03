// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// engine_import_probe's OneLibrary writes, in a translation unit of their
// own: the OneLibrary writer's header brings in SQLCipher's names, which
// collide with the plain <sqlite3.h> the rest of the probe reads m.db with.

#include "engine_import_probe_onelibrary.hpp"

#include "infrastructure/onelibrary/onelibrary_cue_writer.hpp"
#include "infrastructure/onelibrary/onelibrary_key.hpp"
#include "infrastructure/onelibrary/onelibrary_reader.hpp"
#include "infrastructure/onelibrary/sqlcipher_dyn.hpp"

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

std::vector<domain::Track> readOneLibraryTracks(const std::string &pioneerRoot)
{
    if (!hasOneLibrary(pioneerRoot)) {
        return {};
    }
    infrastructure::onelibrary::OneLibraryReader reader(pioneerRoot);
    return reader.readAll();
}

std::vector<OneLibraryAnnotation> readOneLibraryAnnotations(const std::string &pioneerRoot)
{
    std::vector<OneLibraryAnnotation> out;
    if (!hasOneLibrary(pioneerRoot)) {
        return out;
    }
    using namespace infrastructure::onelibrary;
    SqlCipherLibrary lib;
    SqlCipherDb db(lib, OneLibraryCueWriter::dbPathFor(pioneerRoot), /*readOnly=*/true);
    db.exec("PRAGMA key = '" + deriveOneLibraryKey() + "';");
    SqlCipherStatement select(db, "SELECT path, rating, djComment FROM content ORDER BY content_id");
    while (select.step()) {
        out.push_back(OneLibraryAnnotation{select.columnText(0),
                                           select.columnIsNull(1) ? "NULL" : std::to_string(select.columnInt64(1)),
                                           select.columnIsNull(2) ? "NULL" : select.columnText(2)});
    }
    return out;
}

void writeOneLibraryAnnotation(const std::string &pioneerRoot, const std::string &filePath, int stars,
                               const std::string &comment)
{
    infrastructure::onelibrary::OneLibraryCueWriter writer(pioneerRoot);
    writer.writeAnnotationForPath(filePath, stars, comment);
}

}  // namespace seabass::probe
