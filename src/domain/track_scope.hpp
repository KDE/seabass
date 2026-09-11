// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// (format, sourceId) -- the same pairing PlaybackController's own doc
// comment already notes is required to identify a track uniquely (sourceId
// alone isn't unique across formats).
using TrackId = std::pair<std::string, std::string>;

// Which subset of a scanned library an operation (Sync, Clean Up, ...)
// should act on. Deliberately generic rather than named after any one
// caller's own concept (e.g. "playlist filter") -- every feature that
// scopes itself to less than the whole library goes through this one
// type, so a caller never has to re-decide what "scoped to a playlist"
// or "scoped to a search" means.
class TrackScope
{
public:
    static TrackScope all();
    static TrackScope playlist(std::string name);
    // Case-insensitive substring match against title or artist. ASCII-only
    // case folding (std::tolower per byte) -- fine for the tests and any
    // ASCII-only caller, but NOT a drop-in for a user-facing search box:
    // ScanController::search() deliberately keeps its own QString-based
    // matching instead of calling this, specifically because real music
    // metadata (accented artist/title names) needs Unicode-aware case
    // folding, which domain/ has no dependency-free way to do correctly.
    static TrackScope search(std::string query);
    // An explicit set of tracks, unrelated to any playlist/search
    // criteria -- e.g. a manual multi-select in a future UI ("act on
    // just these"). filterByScope() only ever checks membership; how the
    // set was chosen (by hand, at random, ...) is entirely up to the
    // caller.
    static TrackScope arbitrary(std::set<TrackId> ids);

    bool isAll() const { return m_kind == Kind::All; }
    bool matches(const Track &track) const;

private:
    enum class Kind
    {
        All,
        Playlist,
        Search,
        Arbitrary
    };

    Kind m_kind = Kind::All;
    std::string m_value;     // playlist name / search query
    std::set<TrackId> m_ids;  // Arbitrary only

    TrackScope() = default;
};

// tracks filtered down to whatever scope.matches() accepts, preserving
// their original relative order. scope.isAll() is never a no-op shortcut
// here beyond what matches() itself already does -- TrackScope::all()'s
// own matches() always returns true, so the loop already degenerates to a
// full copy without needing a special case.
std::vector<Track> filterByScope(const std::vector<Track> &tracks, const TrackScope &scope);

}  // namespace seabass::domain
