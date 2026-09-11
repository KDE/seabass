// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>

namespace seabass::infrastructure::onelibrary
{

struct OneLibraryAnonymizationResult
{
    int tracksScrubbed = 0;
    int artistsRenamed = 0;
    int playlistsRenamed = 0;
    int cueCommentsScrubbed = 0;
    int albumsRenamed = 0;
    int genresRenamed = 0;
    int labelsRenamed = 0;
    int myTagsRenamed = 0;
    std::string errorMessage;
};

// Scrubs the identifying text out of a copy of exportLibrary.db, the
// Device Library Plus mirror that lives beside export.pdb.
//
// This database used to be swept into the anonymized export whole by a
// blanket directory copy, which meant shipping the complete real library
// behind a key this project's own source derives. It was then excluded
// outright, which was safe but left the OneLibrary write path -- the one
// costing 235 ms per item -- with no real-data coverage at all. This is
// what lets it come back scrubbed.
//
// Placeholders are derived from each track's real filename, the same key
// the rekordbox and Engine anonymizers use, so one real track gets the
// same placeholder in all three catalogs. Cross-catalog matching is keyed
// on exactly those fields, so anything else would make a sync test against
// anonymized data look broken while the matcher was fine.
//
// Operates in place on `dbPath`, which must already be a copy. It never
// touches a stick.
OneLibraryAnonymizationResult anonymizeOneLibraryDatabase(const std::string &dbPath);

}  // namespace seabass::infrastructure::onelibrary
