// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "domain/engine_update_planning.hpp"
#include "domain/rekordbox_baseline.hpp"
#include "domain/track.hpp"

namespace seabass::application
{

// Sync after Rekordbox Export's input, put together once for every caller
// (seabass-cli sync-after-export, the GUI's controller): the two catalogs
// as already read, and what only the stick can say, as data. Reading the
// stick is the caller's (infrastructure/local/engine_update_stick_facts
// does it for both); EngineUpdatePlanner::plan never reads a file.
// See docs/sync-after-rekordbox-export-plan.md.
struct EngineUpdateStickFacts
{
    // The directory both catalogs sit in (paths::stickRootForCatalogPath).
    // Track::filePath is made relative to it.
    std::string stickRoot;
    // export.pdb's playlist tree, folders included
    // (rekordbox::rekordboxPlaylistTree).
    std::vector<domain::PlaylistInfo> rekordboxPlaylists;
    // Every Engine playlist and folder with its count at path
    // (engine::listEnginePlaylists).
    std::vector<domain::EnginePlaylistInfo> enginePlaylists;
    // nullopt: the stick has none, and the planner falls back. A baseline
    // that exists and cannot be read is the caller's error to report,
    // never a nullopt here.
    std::optional<domain::RekordboxBaseline> baseline;
    // export.pdb's sequence now.
    std::uint64_t currentSequence = 0;
    // Track.pdbImportKey by Engine sourceId (engine::readEnginePdbImportKeys).
    std::map<std::string, std::int64_t> enginePdbImportKey;
    // Whether the stick has a file at a stick-relative path. Unset: every
    // file is taken to be there, which proposes adds for files that are
    // not; a caller with a stick passes fileExistsUnder(stickRoot).
    std::function<bool(const std::string &stickRelativePath)> fileExists;
};

// `filePath` relative to `stickRoot`, with forward slashes, worked out
// on the strings alone (no filesystem access, so it answers the same for
// a stick that is gone). Empty when either is empty or the file is not
// under the root: such a row has no key, and baselineFrom reports it
// rather than keying it on its bare filename.
std::string stickRelativePathOf(const std::string &filePath, const std::string &stickRoot);

// A fileExists predicate for the stick at `stickRoot`: a stat per call,
// true for a regular file at that stick-relative path.
std::function<bool(const std::string &)> fileExistsUnder(const std::string &stickRoot);

// The planner's input: the tracks as given (rekordbox with cues and
// playlists, Engine with cues read at each file's own sample rate), the
// facts above, stickRelativeOf from stickRelativePathOf and pathKeyOf
// from normalizedPathKey, the keys the baseline was recorded with.
domain::EngineUpdateInput buildEngineUpdateInput(std::vector<domain::Track> rekordbox,
                                                 std::vector<domain::Track> engine, EngineUpdateStickFacts facts);

}  // namespace seabass::application
