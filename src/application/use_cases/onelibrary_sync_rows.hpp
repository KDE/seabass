// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <set>
#include <string>
#include <vector>

#include "application/path_key.hpp"
#include "domain/track.hpp"

namespace seabass::application
{

// The OneLibrary rows Sync pairs with Engine on their own: those whose
// cues no DeviceLibrary row in `deviceLibrary` already speaks for.
//
// A OneLibrary row's cues are its analysis file's (issue #59), and a
// DeviceLibrary row naming the same analysis file has the same cues,
// written by the same writer. The Engine <-> DeviceLibrary pair already
// decides that file; planning the OneLibrary row against Engine as well
// would decide it twice, and whichever write landed last would win. So a
// row is left out when DeviceLibrary names its analysis file, and also
// when DeviceLibrary lists its audio file: every DeviceLibrary cue write
// is mirrored into the OneLibrary rows at that path (and their analysis
// files), so those rows are spoken for too.
//
// What is left are the tracks only OneLibrary holds (about 480 of 1644 on
// WHALESHARK2), which keep their own pairing with Engine, through their
// own analysis file.
//
// Analysis paths compare without regard to ASCII case: both rekordbox
// catalogs spell them, and the stick's FAT/exFAT does not distinguish.
inline std::vector<domain::Track> oneLibraryRowsToPairWithEngine(const std::vector<domain::Track> &deviceLibrary,
                                                                 const std::vector<domain::Track> &oneLibrary)
{
    const auto analysisKey = [](std::string path) {
        for (char &c : path) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return path;
    };
    std::set<std::string> deviceLibraryPaths;
    std::set<std::string> deviceLibraryAnalysisFiles;
    for (const auto &track : deviceLibrary) {
        if (!track.filePath.empty()) {
            deviceLibraryPaths.insert(normalizedPathKey(track.filePath));
        }
        if (!track.analysisFile.empty()) {
            deviceLibraryAnalysisFiles.insert(analysisKey(track.analysisFile));
        }
    }
    std::vector<domain::Track> rows;
    for (const auto &track : oneLibrary) {
        if (!track.filePath.empty() && deviceLibraryPaths.count(normalizedPathKey(track.filePath))) {
            continue;
        }
        if (!track.analysisFile.empty() && deviceLibraryAnalysisFiles.count(analysisKey(track.analysisFile))) {
            continue;
        }
        rows.push_back(track);
    }
    return rows;
}

}  // namespace seabass::application
