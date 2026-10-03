// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <optional>
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

// Every track exportLibrary.db lists, through Seabass's OneLibraryReader.
// Empty when the stick has none.
std::vector<domain::Track> readOneLibraryTracks(const std::string &pioneerRoot);

// content.rating and content.djComment per content.path (stick-relative,
// with its leading slash), read straight from the table: the reader does
// not carry them. "NULL" for a NULL value.
struct OneLibraryAnnotation
{
    std::string path;
    std::string rating;
    std::string comment;
};
std::vector<OneLibraryAnnotation> readOneLibraryAnnotations(const std::string &pioneerRoot);

// Rating (stars) and comment into exportLibrary.db, through Seabass's own
// OneLibraryCueWriter::writeAnnotationForPath.
void writeOneLibraryAnnotation(const std::string &pioneerRoot, const std::string &filePath, int stars,
                               const std::string &comment);

}  // namespace seabass::probe
