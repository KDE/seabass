// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/track_scope.hpp"

#include <algorithm>
#include <cctype>

namespace seabass::domain
{

namespace
{

std::string toLowerAscii(const std::string &s)
{
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool inPlaylist(const Track &track, const std::string &playlistName)
{
    for (const auto &membership : track.playlists) {
        if (membership.name == playlistName) {
            return true;
        }
    }
    return false;
}

}  // namespace

TrackScope TrackScope::all()
{
    TrackScope scope;
    scope.m_kind = Kind::All;
    return scope;
}

TrackScope TrackScope::playlist(std::string name)
{
    TrackScope scope;
    scope.m_kind = Kind::Playlist;
    scope.m_value = std::move(name);
    return scope;
}

TrackScope TrackScope::search(std::string query)
{
    TrackScope scope;
    scope.m_kind = Kind::Search;
    // Lowercased once here rather than per-track in matches() below --
    // the query itself never changes for the lifetime of this scope, only
    // title/artist (which do still need lowering per track) vary.
    scope.m_value = toLowerAscii(query);
    return scope;
}

TrackScope TrackScope::arbitrary(std::set<TrackId> ids)
{
    TrackScope scope;
    scope.m_kind = Kind::Arbitrary;
    scope.m_ids = std::move(ids);
    return scope;
}

bool TrackScope::matches(const Track &track) const
{
    switch (m_kind) {
        case Kind::All:
            return true;
        case Kind::Playlist:
            return inPlaylist(track, m_value);
        case Kind::Search:
            // m_value is already lowercased -- see TrackScope::search().
            return toLowerAscii(track.title).find(m_value) != std::string::npos
                || toLowerAscii(track.artist).find(m_value) != std::string::npos;
        case Kind::Arbitrary: {
            // Any of the file's rows will do. A track that has been
            // collapsed into a file (see
            // application::collapseCatalogRows) carries one base row
            // plus the rows the other formats wrote for the same file,
            // and a caller that picked "this track" picked the file --
            // it cannot be expected to know, or care, which format's row
            // id ended up as the base. Selecting the Engine row and
            // being told the file is out of scope because its base row
            // is rekordbox's would be nonsense.
            //
            // catalogRows is empty on anything not collapsed, so a
            // caller working in one format at a time is unaffected.
            if (m_ids.count(TrackId{track.format, track.sourceId}) > 0) {
                return true;
            }
            for (const auto &row : track.catalogRows) {
                if (m_ids.count(TrackId{row.format, row.sourceId}) > 0) {
                    return true;
                }
            }
            return false;
        }
    }
    return false;
}

std::vector<Track> filterByScope(const std::vector<Track> &tracks, const TrackScope &scope)
{
    std::vector<Track> result;
    result.reserve(tracks.size());
    for (const auto &track : tracks) {
        if (scope.matches(track)) {
            result.push_back(track);
        }
    }
    return result;
}

}  // namespace seabass::domain
