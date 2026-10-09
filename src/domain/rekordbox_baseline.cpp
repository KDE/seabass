// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/rekordbox_baseline.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string_view>
#include <unordered_map>

namespace seabass::domain
{

namespace
{

// The whole of `text` as a number, or nothing: "12x" and "" are not 12.
template<typename Number>
std::optional<Number> wholeNumber(std::string_view text)
{
    Number value{};
    const auto *end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    if (text.empty() || result.ec != std::errc() || result.ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::string parentPath(const std::string &path)
{
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// Splits off the first field of `rest` at ':' and advances `rest` past it;
// nullopt when there is no ':' left.
std::optional<std::string_view> takeField(std::string_view &rest)
{
    const auto colon = rest.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const auto field = rest.substr(0, colon);
    rest.remove_prefix(colon + 1);
    return field;
}

}  // namespace

const BaselineTrack *RekordboxBaseline::findTrack(const std::string &pathKey) const
{
    const auto it = std::find_if(tracks.begin(), tracks.end(), [&](const BaselineTrack &t) {
        return t.pathKey == pathKey;
    });
    return it == tracks.end() ? nullptr : &*it;
}

const BaselinePlaylist *RekordboxBaseline::findPlaylist(std::uint32_t id) const
{
    const auto it = std::find_if(playlists.begin(), playlists.end(), [&](const BaselinePlaylist &p) {
        return p.id == id;
    });
    return it == playlists.end() ? nullptr : &*it;
}

const BaselinePlaylist *RekordboxBaseline::findPlaylistByPath(const std::string &path) const
{
    const auto it = std::find_if(playlists.begin(), playlists.end(), [&](const BaselinePlaylist &p) {
        return p.path == path;
    });
    return it == playlists.end() ? nullptr : &*it;
}

RekordboxBaseline baselineFrom(const std::vector<Track> &rekordbox, const std::vector<PlaylistInfo> &playlists,
                               std::uint64_t pdbSequence,
                               const std::function<std::string(const std::string &)> &stickRelativeOf,
                               const std::function<std::string(const std::string &)> &pathKeyOf, BaselineGaps *gaps)
{
    RekordboxBaseline baseline;
    baseline.pdbSequence = pdbSequence;

    std::unordered_map<std::string, std::size_t> playlistIndexByPath;
    std::unordered_map<std::uint32_t, std::size_t> playlistIndexById;
    baseline.playlists.reserve(playlists.size());
    for (const auto &info : playlists) {
        BaselinePlaylist playlist;
        playlist.id = info.id;
        playlist.folder = info.folder;
        playlist.path = info.path;
        playlistIndexByPath.emplace(info.path, baseline.playlists.size());
        playlistIndexById.emplace(info.id, baseline.playlists.size());
        baseline.playlists.push_back(std::move(playlist));
    }
    for (auto &playlist : baseline.playlists) {
        const std::string parent = parentPath(playlist.path);
        if (parent.empty()) {
            continue;
        }
        const auto it = playlistIndexByPath.find(parent);
        if (it == playlistIndexByPath.end()) {
            if (gaps) {
                gaps->orphanPlaylists.push_back(playlist.path);
            }
            continue;
        }
        playlist.parentId = baseline.playlists[it->second].id;
    }

    // Members gathered with their position and the order they were met,
    // sorted once all tracks are in.
    struct Entry
    {
        int position;
        std::size_t seen;
        std::string pathKey;
    };
    std::vector<std::vector<Entry>> entries(baseline.playlists.size());
    std::size_t seen = 0;

    baseline.tracks.reserve(rekordbox.size());
    for (const auto &track : rekordbox) {
        const std::string relative = stickRelativeOf(track.filePath);
        const std::string key = relative.empty() ? std::string() : pathKeyOf(relative);
        if (key.empty()) {
            if (gaps) {
                gaps->unkeyedTracks.push_back(track.sourceId);
            }
            continue;
        }

        BaselineTrack row;
        row.pathKey = key;
        row.stickRelativePath = relative;
        row.analysisFile = track.analysisFile;
        row.pdbId = wholeNumber<std::uint32_t>(track.sourceId).value_or(0);
        row.rating = track.rating;
        row.ratingOrigin = ValueOrigin::Unknown;
        row.comment = track.comment;
        row.bpm = track.bpm;
        row.durationMs = std::llround(track.durationSeconds * 1000.0);
        row.cues.reserve(track.cues.size());
        for (const auto &cue : track.cues) {
            row.cues.push_back(BaselineCue{cue, ValueOrigin::Unknown});
        }
        baseline.tracks.push_back(std::move(row));

        for (const auto &membership : track.playlists) {
            // By id where the reader gave one: two playlists can share a
            // path, and by name every entry would land on the first.
            std::optional<std::size_t> index;
            if (const auto byId = playlistIndexById.find(membership.playlistId);
                membership.playlistId != 0 && byId != playlistIndexById.end()) {
                index = byId->second;
            } else if (const auto byPath = playlistIndexByPath.find(membership.name);
                       byPath != playlistIndexByPath.end()) {
                index = byPath->second;
            }
            if (!index || baseline.playlists[*index].folder) {
                if (gaps) {
                    gaps->unknownMemberships.push_back(track.sourceId + " in " + membership.name);
                }
                continue;
            }
            entries[*index].push_back(Entry{membership.position, seen++, key});
        }
    }

    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto &list = entries[i];
        // Unknown positions (-1) after the known ones; `seen` keeps the
        // reader's order among ties, so this needs no stable sort.
        std::sort(list.begin(), list.end(), [](const Entry &a, const Entry &b) {
            const bool aUnknown = a.position < 0;
            const bool bUnknown = b.position < 0;
            if (aUnknown != bUnknown) {
                return bUnknown;
            }
            if (!aUnknown && a.position != b.position) {
                return a.position < b.position;
            }
            return a.seen < b.seen;
        });
        auto &members = baseline.playlists[i].members;
        members.reserve(list.size());
        for (auto &entry : list) {
            members.push_back(std::move(entry.pathKey));
        }
    }
    return baseline;
}

std::string itemKey(const ItemKey &key)
{
    switch (key.kind) {
    case ItemKey::Kind::Track:
        return "track:" + key.pathKey;
    case ItemKey::Kind::Playlist:
        return "playlist:" + std::to_string(key.playlistId);
    case ItemKey::Kind::Member:
        return "member:" + std::to_string(key.playlistId) + ":" + key.pathKey;
    case ItemKey::Kind::Rating:
        return "rating:" + key.pathKey;
    case ItemKey::Kind::Comment:
        return "comment:" + key.pathKey;
    case ItemKey::Kind::HotCue:
        return "cue:hot:" + std::to_string(key.pad) + ":" + key.pathKey;
    case ItemKey::Kind::MemoryCue:
        return "cue:memory:" + std::to_string(key.positionMs) + ":" + key.pathKey;
    case ItemKey::Kind::MemoryLoop:
        return "cue:loop:" + std::to_string(key.positionMs) + ":" + key.pathKey;
    }
    return {};
}

std::optional<ItemKey> parseItemKey(const std::string &text)
{
    std::string_view rest(text);
    const auto head = takeField(rest);
    if (!head) {
        return std::nullopt;
    }
    ItemKey key;
    if (*head == "playlist") {
        const auto id = wholeNumber<std::uint32_t>(rest);
        if (!id) {
            return std::nullopt;
        }
        key.kind = ItemKey::Kind::Playlist;
        key.playlistId = *id;
        return key;
    }
    if (*head == "track" || *head == "rating" || *head == "comment") {
        key.kind = *head == "track" ? ItemKey::Kind::Track
            : *head == "rating"     ? ItemKey::Kind::Rating
                                    : ItemKey::Kind::Comment;
    } else if (*head == "member") {
        const auto idField = takeField(rest);
        const auto id = idField ? wholeNumber<std::uint32_t>(*idField) : std::nullopt;
        if (!id) {
            return std::nullopt;
        }
        key.kind = ItemKey::Kind::Member;
        key.playlistId = *id;
    } else if (*head == "cue") {
        const auto kind = takeField(rest);
        const auto numberField = kind ? takeField(rest) : std::nullopt;
        if (!numberField) {
            return std::nullopt;
        }
        if (*kind == "hot") {
            const auto pad = wholeNumber<int>(*numberField);
            if (!pad || *pad < 1) {
                return std::nullopt;
            }
            key.kind = ItemKey::Kind::HotCue;
            key.pad = *pad;
        } else if (*kind == "memory" || *kind == "loop") {
            const auto ms = wholeNumber<std::int64_t>(*numberField);
            if (!ms) {
                return std::nullopt;
            }
            key.kind = *kind == "memory" ? ItemKey::Kind::MemoryCue : ItemKey::Kind::MemoryLoop;
            key.positionMs = *ms;
        } else {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    if (rest.empty()) {
        return std::nullopt;
    }
    key.pathKey = std::string(rest);
    return key;
}

std::string trackItemKey(const std::string &pathKey)
{
    return itemKey(ItemKey{ItemKey::Kind::Track, pathKey});
}

std::string playlistItemKey(std::uint32_t id)
{
    ItemKey key;
    key.kind = ItemKey::Kind::Playlist;
    key.playlistId = id;
    return itemKey(key);
}

std::string memberItemKey(std::uint32_t playlistId, const std::string &pathKey)
{
    ItemKey key{ItemKey::Kind::Member, pathKey};
    key.playlistId = playlistId;
    return itemKey(key);
}

std::string ratingItemKey(const std::string &pathKey)
{
    return itemKey(ItemKey{ItemKey::Kind::Rating, pathKey});
}

std::string commentItemKey(const std::string &pathKey)
{
    return itemKey(ItemKey{ItemKey::Kind::Comment, pathKey});
}

std::string cueItemKey(const std::string &pathKey, const CuePoint &cue)
{
    ItemKey key{ItemKey::Kind::HotCue, pathKey};
    if (cue.kind == CuePoint::Kind::Hot) {
        key.pad = cue.hotCueNumber;
    } else {
        key.kind = cue.isLoop ? ItemKey::Kind::MemoryLoop : ItemKey::Kind::MemoryCue;
        key.positionMs = std::llround(cue.positionMs);
    }
    return itemKey(key);
}

}  // namespace seabass::domain
