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

void BaselineTrackIndex::invalidate() const noexcept
{
    std::lock_guard lock(m_mutex);
    m_built = false;
    m_data = nullptr;
    m_size = 0;
    m_rows.clear();
}

void BaselineTrackIndex::buildLocked(const std::vector<BaselineTrack> &tracks) const
{
    m_rows.clear();
    m_rows.reserve(tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        m_rows[tracks[i].pathKey].push_back(i);
    }
    m_data = tracks.data();
    m_size = tracks.size();
    m_built = true;
}

const std::vector<std::size_t> &BaselineTrackIndex::rowsOf(const std::vector<BaselineTrack> &tracks,
                                                           const std::string &pathKey) const
{
    static const std::vector<std::size_t> none;
    std::lock_guard lock(m_mutex);
    if (!m_built || m_data != tracks.data() || m_size != tracks.size()) {
        buildLocked(tracks);
    }
    auto it = m_rows.find(pathKey);
    if (it != m_rows.end() && tracks[it->second.front()].pathKey != pathKey) {
        // The rows moved under the index without their count changing.
        buildLocked(tracks);
        it = m_rows.find(pathKey);
    }
    return it == m_rows.end() ? none : it->second;
}

const std::vector<std::size_t> &RekordboxBaseline::trackRowsOf(const std::string &pathKey) const
{
    return trackIndex.rowsOf(tracks, pathKey);
}

const BaselineTrack *RekordboxBaseline::findTrack(const std::string &pathKey) const
{
    const auto &rows = trackRowsOf(pathKey);
    return rows.empty() ? nullptr : &tracks[rows.front()];
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
    std::vector<std::vector<MemberEntry>> entries(baseline.playlists.size());
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
            entries[*index].push_back(MemberEntry{membership.position, seen++, key});
        }
    }

    for (std::size_t i = 0; i < entries.size(); ++i) {
        baseline.playlists[i].members = orderedMembers(std::move(entries[i]));
    }
    return baseline;
}

std::vector<std::string> orderedMembers(std::vector<MemberEntry> entries)
{
    // `seen` keeps the reader's order among ties, so this needs no stable
    // sort.
    std::sort(entries.begin(), entries.end(), [](const MemberEntry &a, const MemberEntry &b) {
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
    std::vector<std::string> members;
    members.reserve(entries.size());
    for (auto &entry : entries) {
        members.push_back(std::move(entry.pathKey));
    }
    return members;
}

std::vector<std::string> firstOccurrences(const std::vector<std::string> &keys)
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto &key : keys) {
        if (seen.insert(key).second) {
            out.push_back(key);
        }
    }
    return out;
}

std::optional<int> effectiveRating(std::optional<int> rating)
{
    return rating && *rating > 0 ? rating : std::nullopt;
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
    case ItemKey::Kind::Order:
        return "order:" + std::to_string(key.playlistId);
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
    if (*head == "playlist" || *head == "order") {
        const auto id = wholeNumber<std::uint32_t>(rest);
        if (!id) {
            return std::nullopt;
        }
        key.kind = *head == "playlist" ? ItemKey::Kind::Playlist : ItemKey::Kind::Order;
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

std::string orderItemKey(std::uint32_t playlistId)
{
    ItemKey key;
    key.kind = ItemKey::Kind::Order;
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

namespace
{

// Colour and comment do not decide a cue's identity (the rule everywhere
// in Seabass): two cues are the same when they sit at the same place.
bool sameCue(const CuePoint &a, const CuePoint &b)
{
    return a.kind == b.kind && a.hotCueNumber == b.hotCueNumber && a.positionMs == b.positionMs
        && a.isLoop == b.isLoop && (!a.isLoop || a.loopEndMs == b.loopEndMs);
}

int keyOrder(ItemKey::Kind kind)
{
    switch (kind) {
    case ItemKey::Kind::Track:
        return 0;
    case ItemKey::Kind::Playlist:
        return 1;
    case ItemKey::Kind::Member:
        return 2;
    case ItemKey::Kind::Order:
        return 3;
    default:
        return 4;
    }
}

void keepTrack(RekordboxBaseline &next, const RekordboxBaseline *previous, const std::string &pathKey)
{
    const bool inNext = next.findTrack(pathKey) != nullptr;
    const bool inPrevious = previous && previous->findTrack(pathKey) != nullptr;
    if (inNext && !inPrevious) {
        std::erase_if(next.tracks, [&](const BaselineTrack &t) { return t.pathKey == pathKey; });
        next.trackIndex.invalidate();
    } else if (!inNext && inPrevious) {
        for (const std::size_t i : previous->trackRowsOf(pathKey)) {
            next.tracks.push_back(previous->tracks[i]);
        }
        next.trackIndex.invalidate();
    }
}

void keepPlaylist(RekordboxBaseline &next, const RekordboxBaseline *previous, std::uint32_t id)
{
    const BaselinePlaylist *before = previous ? previous->findPlaylist(id) : nullptr;
    const auto it = std::find_if(next.playlists.begin(), next.playlists.end(), [&](const BaselinePlaylist &p) {
        return p.id == id;
    });
    if (it == next.playlists.end()) {
        if (before) {
            BaselinePlaylist restored = *before;
            restored.members.clear();
            next.playlists.push_back(std::move(restored));
        }
        return;
    }
    if (!before) {
        next.playlists.erase(it);
        return;
    }
    it->path = before->path;
    it->parentId = before->parentId;
    it->folder = before->folder;
}

void keepMember(RekordboxBaseline &next, const RekordboxBaseline *previous, std::uint32_t id,
                const std::string &pathKey)
{
    const auto it = std::find_if(next.playlists.begin(), next.playlists.end(), [&](const BaselinePlaylist &p) {
        return p.id == id;
    });
    if (it == next.playlists.end()) {
        return;  // no playlist to hold it
    }
    std::erase(it->members, pathKey);
    const BaselinePlaylist *before = previous ? previous->findPlaylist(id) : nullptr;
    if (!before) {
        return;
    }
    for (std::size_t i = 0; i < before->members.size(); ++i) {
        if (before->members[i] == pathKey) {
            const std::size_t at = std::min(i, it->members.size());
            it->members.insert(it->members.begin() + static_cast<std::ptrdiff_t>(at), pathKey);
        }
    }
}

void keepOrder(RekordboxBaseline &next, const RekordboxBaseline *previous, std::uint32_t id)
{
    const BaselinePlaylist *before = previous ? previous->findPlaylist(id) : nullptr;
    const auto it = std::find_if(next.playlists.begin(), next.playlists.end(), [&](const BaselinePlaylist &p) {
        return p.id == id;
    });
    if (!before || it == next.playlists.end()) {
        return;  // no recorded order, or no playlist to hold one
    }
    std::set<std::string> seen;
    std::set<std::string> inNext;
    for (const auto &key : it->members) {
        inNext.insert(key);
    }
    std::vector<std::string> order;  // previous's, first entries, of members next has
    for (const auto &key : before->members) {
        if (inNext.contains(key) && seen.insert(key).second) {
            order.push_back(key);
        }
    }
    const std::set<std::string> recorded(order.begin(), order.end());
    seen.clear();
    std::size_t at = 0;
    for (auto &key : it->members) {
        if (recorded.contains(key) && seen.insert(key).second) {
            key = order[at++];
        }
    }
}

void keepValue(RekordboxBaseline &next, const RekordboxBaseline *previous, const ItemKey &key,
               const std::string &text)
{
    const BaselineTrack *before = previous ? previous->findTrack(key.pathKey) : nullptr;
    for (const std::size_t i : next.trackRowsOf(key.pathKey)) {
        BaselineTrack &row = next.tracks[i];
        switch (key.kind) {
        case ItemKey::Kind::Rating:
            row.rating = before ? before->rating : std::nullopt;
            row.ratingOrigin = before ? before->ratingOrigin : ValueOrigin::Unknown;
            break;
        case ItemKey::Kind::Comment:
            row.comment = before ? before->comment : std::string();
            break;
        default:
            std::erase_if(row.cues, [&](const BaselineCue &c) { return cueItemKey(key.pathKey, c.cue) == text; });
            if (before) {
                for (const auto &cue : before->cues) {
                    if (cueItemKey(key.pathKey, cue.cue) == text) {
                        row.cues.push_back(cue);
                    }
                }
            }
            break;
        }
    }
}

}  // namespace

std::vector<std::string> recordSeabassWrites(RekordboxBaseline &baseline, const std::vector<SeabassWrite> &writes)
{
    std::vector<std::string> unlisted;
    for (const auto &write : writes) {
        const std::vector<std::size_t> &rows = baseline.trackRowsOf(write.pathKey);
        const bool found = !rows.empty();
        for (const std::size_t i : rows) {
            BaselineTrack &row = baseline.tracks[i];
            if (write.cues) {
                row.cues.clear();
                for (const auto &cue : *write.cues) {
                    row.cues.push_back(BaselineCue{cue, ValueOrigin::Seabass});
                }
            }
            if (write.rating) {
                row.rating = *write.rating > 0 ? write.rating : std::nullopt;
                row.ratingOrigin = ValueOrigin::Seabass;
            }
        }
        if (!found) {
            unlisted.push_back(write.pathKey);
        }
    }
    return unlisted;
}

void keepPreviousItems(RekordboxBaseline &next, const RekordboxBaseline *previous, const std::set<std::string> &keys)
{
    std::vector<std::pair<ItemKey, std::string>> parsed;
    for (const auto &text : keys) {
        if (auto key = parseItemKey(text)) {
            parsed.emplace_back(std::move(*key), text);
        }
    }
    std::stable_sort(parsed.begin(), parsed.end(), [](const auto &a, const auto &b) {
        return keyOrder(a.first.kind) < keyOrder(b.first.kind);
    });
    for (const auto &[key, text] : parsed) {
        switch (key.kind) {
        case ItemKey::Kind::Track:
            keepTrack(next, previous, key.pathKey);
            break;
        case ItemKey::Kind::Playlist:
            keepPlaylist(next, previous, key.playlistId);
            break;
        case ItemKey::Kind::Member:
            keepMember(next, previous, key.playlistId, key.pathKey);
            break;
        case ItemKey::Kind::Order:
            keepOrder(next, previous, key.playlistId);
            break;
        default:
            keepValue(next, previous, key, text);
            break;
        }
    }
}

RekordboxBaseline nextBaseline(const RekordboxBaseline *previous, const std::vector<Track> &rekordboxNow,
                               const std::vector<PlaylistInfo> &playlists, std::uint64_t sequence,
                               const std::set<std::string> &offered, const std::set<std::string> &applied,
                               const std::map<std::string, std::string> &declined,
                               const std::function<std::string(const std::string &)> &stickRelativeOf,
                               const std::function<std::string(const std::string &)> &pathKeyOf, BaselineGaps *gaps)
{
    RekordboxBaseline next = baselineFrom(rekordboxNow, playlists, sequence, stickRelativeOf, pathKeyOf, gaps);
    if (previous) {
        next.engineUuid = previous->engineUuid;
        for (auto &row : next.tracks) {
            const BaselineTrack *before = previous->findTrack(row.pathKey);
            if (!before) {
                continue;
            }
            if (row.rating == before->rating) {
                row.ratingOrigin = before->ratingOrigin;
            }
            for (auto &cue : row.cues) {
                const auto same = std::find_if(before->cues.begin(), before->cues.end(), [&](const BaselineCue &b) {
                    return sameCue(b.cue, cue.cue);
                });
                if (same != before->cues.end()) {
                    cue.origin = same->origin;
                }
            }
        }
    }

    std::set<std::string> kept;
    for (const auto &key : offered) {
        if (!applied.contains(key)) {
            kept.insert(key);
        }
    }
    for (const auto &[key, hash] : declined) {
        kept.insert(key);
    }
    keepPreviousItems(next, previous, kept);

    next.declined = declined;
    if (previous) {
        for (const auto &[key, hash] : previous->declined) {
            if (kept.contains(key)) {
                next.declined.emplace(key, hash);  // a new decline of the same key wins
            }
        }
    }
    return next;
}

}  // namespace seabass::domain
