// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <chrono>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "application/path_key.hpp"
#include "application/ports/progress_reporter.hpp"
#include "domain/sync_planning.hpp"
#include "domain/track.hpp"
#include "infrastructure/long_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::application
{

// Read-only use case: match tracks across two catalog scans and decide
// what should happen to each pair's cues. Deliberately generic (tracksA/
// tracksB, not named after specific formats) -- callers run this once
// per pair of catalogs actually present on a stick (rekordbox<->Engine,
// rekordbox<->OneLibrary, Engine<->OneLibrary), see
// gui::SyncController::analyze(). Applying a plan is a separate step
// (see cli/), since only some directions are writable.
class SyncLibraries
{
public:
    // Whether two paths open one file, as the filesystem says: one inode
    // through both. On FAT32 and exFAT "Akki (De)/x.mp3" and
    // "AKKI (DE)/x.mp3" do; on a case-sensitive filesystem they are two
    // files or one name for nothing. A path that cannot be opened, or
    // decoded, is not the same file as anything.
    static bool sameFileOnDisk(const std::string &a, const std::string &b)
    {
        try {
            std::error_code ec;
            const bool same = std::filesystem::equivalent(infrastructure::longPathSafe(pathFromUtf8(a)),
                                                          infrastructure::longPathSafe(pathFromUtf8(b)), ec);
            return same && !ec;
        } catch (const std::exception &) {
            return false;
        }
    }

    // Two catalogs of one stick: rows under two spellings of one file
    // pair, by normalizedPathKey and the filesystem's word (#86).
    static const domain::StickFiles &stickFiles()
    {
        static const domain::StickFiles files{&normalizedPathKey, &sameFileOnDisk};
        return files;
    }

    // `progress` sees one phase, "Comparing cues", a tick per matched pair.
    std::vector<domain::SyncPlan> execute(const std::vector<domain::Track> &tracksA,
                                           const std::vector<domain::Track> &tracksB,
                                           std::chrono::system_clock::time_point mtimeA,
                                           std::chrono::system_clock::time_point mtimeB,
                                           ProgressReporter &progress = NullProgressReporter::instance())
    {
        std::vector<domain::SyncPlan> plans;
        const auto matches = domain::TrackMatcher::match(tracksA, tracksB, &stickFiles());
        progress.start("Comparing cues", matches.size());
        for (const auto &match : matches) {
            plans.push_back(domain::SyncPlanner::plan(match, mtimeA, mtimeB));
            progress.tick(plans.size());
        }
        progress.finish();
        return plans;
    }
};

}  // namespace seabass::application
