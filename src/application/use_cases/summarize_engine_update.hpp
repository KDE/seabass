// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "domain/rekordbox_baseline.hpp"
#include "domain/track.hpp"

namespace seabass::application
{

// What the stick card's badge says about Sync after Rekordbox Export, in
// one word: nothing to do, cues to look at, or a library change Engine
// has not caught up with. See docs/sync-after-rekordbox-export-plan.md,
// "The page", the badge paragraph.
enum class EngineUpdateNeed { None, Cues, Library };

// The role's spelling: "", "cues", "library".
std::string engineUpdateNeedName(EngineUpdateNeed need);

// The badge's cheap answer, from the catalogs at the Tracks stage (no
// cues, no file sizes): it never runs EngineUpdatePlanner, which needs
// every cue, and it reads nothing.
//
// With a baseline: Library when rekordbox's track paths, its playlists
// (by id, path and ordered members, from the tracks' memberships) or its
// ratings differ from the baseline; else Cues when export.pdb's sequence
// moved since the baseline was recorded (rekordbox exported, and only
// the cue pass can say whether a cue changed); else None. An item the
// user declined (RekordboxBaseline::declined) at rekordbox's present
// state does not count, as the page leaves it out too. Playlists come
// from memberships only, so an empty playlist rekordbox renamed, made
// or deleted is not seen here; the page sees it.
//
// Without one: Library when a rekordbox track (by pathKey) is not in
// Engine or rekordbox has a playlist path Engine lacks, the additions
// the page would propose checked; else Cues when Engine's import
// counter is not export.pdb's sequence (the player would offer its
// import); else None. Removals and differing values are conflicts on the
// page and never raise the badge on their own.
//
// `stickRelativeOf` and `pathKeyOf` are the planner's (application::
// stickRelativePathOf against the stick root, normalizedPathKey): the
// keys the baseline was recorded with. A row whose key comes out empty
// and a streaming row are left out, as the planner leaves them out.
EngineUpdateNeed summarizeEngineUpdate(const std::vector<domain::Track> &rekordboxTracksStage,
                                       const std::vector<domain::Track> &engineTracksStage,
                                       const std::optional<domain::RekordboxBaseline> &baseline,
                                       std::uint64_t pdbSequence, std::uint64_t engineCounter,
                                       const std::function<std::string(const std::string &)> &stickRelativeOf,
                                       const std::function<std::string(const std::string &)> &pathKeyOf);

}  // namespace seabass::application
