// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <chrono>
#include <set>
#include <string>
#include <vector>

#include "application/path_key.hpp"
#include "application/use_cases/sync_libraries.hpp"
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
// A row that names no analysis file is left out too: a OneLibrary player
// takes its cues from that file, so for such a row there is nothing a
// player reads to compare or to write, only the cue table.
//
// What is left are the tracks only OneLibrary holds (about 480 of 1644 on
// WHALESHARK2), which keep their own pairing with Engine, through their
// own analysis file.
//
// Analysis paths compare by normalizedPathKey, as audio paths do: both
// rekordbox catalogs spell them, export.pdb pads its strings, and the
// stick's filesystem does not tell case apart.
class OneLibraryRowsSpokenFor
{
public:
    explicit OneLibraryRowsSpokenFor(const std::vector<domain::Track> &deviceLibrary)
    {
        for (const auto &track : deviceLibrary) {
            if (!track.filePath.empty()) {
                m_paths.insert(normalizedPathKey(track.filePath));
            }
            if (!track.analysisFile.empty()) {
                m_analysisFiles.insert(normalizedPathKey(track.analysisFile));
            }
        }
    }

    // True when Sync must leave this OneLibrary row out of its own pair
    // with Engine: see oneLibraryRowsToPairWithEngine().
    bool operator()(const domain::Track &row) const
    {
        if (!row.filePath.empty() && m_paths.count(normalizedPathKey(row.filePath))) {
            return true;
        }
        return row.analysisFile.empty() || m_analysisFiles.count(normalizedPathKey(row.analysisFile));
    }

private:
    std::set<std::string> m_paths;
    std::set<std::string> m_analysisFiles;
};

inline std::vector<domain::Track> oneLibraryRowsToPairWithEngine(const std::vector<domain::Track> &deviceLibrary,
                                                                 const std::vector<domain::Track> &oneLibrary)
{
    const OneLibraryRowsSpokenFor spokenFor(deviceLibrary);
    std::vector<domain::Track> rows;
    for (const auto &track : oneLibrary) {
        if (!spokenFor(track)) {
            rows.push_back(track);
        }
    }
    return rows;
}

// The Engine <-> OneLibrary pair's plans, for the rows above only.
//
// Matched against EVERY OneLibrary row, and only then narrowed: taking the
// spoken-for rows out before matching changes what the rest match. An
// Engine copy whose own OneLibrary row was taken out falls through to the
// name fallback and lands on another copy of the song that only OneLibrary
// holds; on a WHALESHARK2 copy (2026-10-03) three Engine copies of one
// title were planned onto one OneLibrary-only row that way, one file's
// cues three times over. Matched first, each Engine copy finds its own row
// by path, and the plan for a spoken-for row is then simply dropped.
inline std::vector<domain::SyncPlan> planEngineWithOneLibrary(const std::vector<domain::Track> &engine,
                                                              const std::vector<domain::Track> &oneLibrary,
                                                              const std::vector<domain::Track> &deviceLibrary,
                                                              std::chrono::system_clock::time_point engineMtime,
                                                              std::chrono::system_clock::time_point oneLibraryMtime)
{
    const OneLibraryRowsSpokenFor spokenFor(deviceLibrary);
    std::vector<domain::SyncPlan> plans;
    for (auto &plan : SyncLibraries().execute(engine, oneLibrary, engineMtime, oneLibraryMtime)) {
        if (!spokenFor(plan.match.trackB)) {
            plans.push_back(std::move(plan));
        }
    }
    return plans;
}

}  // namespace seabass::application
