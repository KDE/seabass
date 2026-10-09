// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/summarize_engine_update.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "domain/engine_update_planning.hpp"

namespace seabass::application
{

namespace
{

using domain::RekordboxBaseline;
using domain::Track;
using KeyFn = std::function<std::string(const std::string &)>;

std::optional<int> effectiveRating(std::optional<int> rating)
{
    return rating && *rating > 0 ? rating : std::nullopt;
}

// The planner's key of a row: empty for a row with no place on the stick.
std::string keyOf(const Track &track, const KeyFn &stickRelativeOf, const KeyFn &pathKeyOf)
{
    const std::string relative = stickRelativeOf ? stickRelativeOf(track.filePath) : track.filePath;
    if (relative.empty()) {
        return {};
    }
    return pathKeyOf ? pathKeyOf(relative) : relative;
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

// One rekordbox playlist as the tracks' memberships give it: members in
// position order, unknown positions after the known ones, ties in the
// order they were met (baselineFrom's rule).
struct MemberList
{
    std::uint32_t id = 0;  // 0: a playlist the baseline does not know by id or path
    std::string path;
    struct Entry
    {
        int position;
        std::size_t seen;
        std::string key;
    };
    std::vector<Entry> entries;

    std::vector<std::string> ordered() const
    {
        std::vector<Entry> sorted = entries;
        std::sort(sorted.begin(), sorted.end(), [](const Entry &a, const Entry &b) {
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
        std::vector<std::string> out;
        out.reserve(sorted.size());
        for (auto &entry : sorted) {
            out.push_back(std::move(entry.key));
        }
        return out;
    }
};

class WithBaseline
{
public:
    WithBaseline(const std::vector<Track> &rekordbox, const RekordboxBaseline &base, const KeyFn &stickRelativeOf,
                 const KeyFn &pathKeyOf)
        : base(base)
    {
        std::size_t seen = 0;
        for (const Track &track : rekordbox) {
            if (!track.streamingSource.empty()) {
                continue;
            }
            const std::string key = keyOf(track, stickRelativeOf, pathKeyOf);
            if (key.empty()) {
                continue;
            }
            // Two rows for one file: the first, as the baseline does.
            if (tracks.emplace(key, &track).second) {
                relative.emplace(key, stickRelativeOf ? stickRelativeOf(track.filePath) : track.filePath);
            }
            for (const auto &membership : track.playlists) {
                std::string listKey;
                std::uint32_t id = membership.playlistId;
                if (id == 0) {
                    const domain::BaselinePlaylist *known = base.findPlaylistByPath(membership.name);
                    id = known ? known->id : 0;
                }
                listKey = id != 0 ? "id:" + std::to_string(id) : "path:" + membership.name;
                MemberList &list = lists[listKey];
                list.id = id;
                list.path = membership.name;
                list.entries.push_back(MemberList::Entry{membership.position, seen++, key});
            }
        }
    }

    bool libraryDiffers() const { return tracksDiffer() || ratingsDiffer() || playlistsDiffer(); }

private:
    const RekordboxBaseline &base;
    std::map<std::string, const Track *> tracks;
    std::map<std::string, std::string> relative;
    std::map<std::string, MemberList> lists;

    // An item counts unless the user declined it at rekordbox's state of
    // it now, recorded as the planner records it (engineUpdateStateHash
    // of the same canonical spelling).
    bool counts(const std::string &itemKey, const std::string &state) const
    {
        const auto it = base.declined.find(itemKey);
        return it == base.declined.end() || it->second != domain::engineUpdateStateHash(state);
    }

    bool tracksDiffer() const
    {
        std::set<std::string> baseKeys;
        for (const auto &track : base.tracks) {
            baseKeys.insert(track.pathKey);
        }
        for (const auto &[key, track] : tracks) {
            if (!baseKeys.count(key) && counts(domain::trackItemKey(key), "track:present:" + relative.at(key))) {
                return true;
            }
        }
        for (const auto &key : baseKeys) {
            if (!tracks.count(key) && counts(domain::trackItemKey(key), "track:absent")) {
                return true;
            }
        }
        return false;
    }

    bool ratingsDiffer() const
    {
        for (const auto &[key, track] : tracks) {
            const domain::BaselineTrack *was = base.findTrack(key);
            if (!was) {
                continue;  // a new track: tracksDiffer's
            }
            const auto now = effectiveRating(track->rating);
            if (now != effectiveRating(was->rating)
                && counts(domain::ratingItemKey(key), "rating:" + (now ? std::to_string(*now) : std::string("none")))) {
                return true;
            }
        }
        return false;
    }

    bool playlistsDiffer() const
    {
        std::set<std::uint32_t> seenIds;
        for (const auto &[listKey, list] : lists) {
            if (list.id == 0) {
                return true;  // a playlist the baseline has never seen, by id or by path
            }
            seenIds.insert(list.id);
            const domain::BaselinePlaylist *was = base.findPlaylist(list.id);
            const std::string listState = "list:" + list.path;
            if (!was || was->folder || was->path != list.path) {
                if (counts(domain::playlistItemKey(list.id), listState)) {
                    return true;
                }
            }
            if (membersDiffer(list.id, list.ordered(), was ? was->members : std::vector<std::string>{})) {
                return true;
            }
        }
        // A list the baseline held members in that no rekordbox track
        // names any more: emptied or deleted, either way every member left.
        // An empty one is not seen here at all.
        for (const auto &was : base.playlists) {
            if (!was.folder && !was.members.empty() && !seenIds.count(was.id)
                && membersDiffer(was.id, {}, was.members)) {
                return true;
            }
        }
        return false;
    }

    // Each member is an item: in or out, and after which member.
    bool membersDiffer(std::uint32_t id, const std::vector<std::string> &now, const std::vector<std::string> &was) const
    {
        const auto stateIn = [&](const std::string &key) {
            const auto it = std::find(now.begin(), now.end(), key);
            if (it == now.end()) {
                return std::string("member:out");
            }
            return "member:in:after:" + (it == now.begin() ? std::string() : *(it - 1));
        };
        const std::vector<std::string> nowFirst = firstOccurrences(now);
        const std::vector<std::string> wasFirst = firstOccurrences(was);
        const auto predecessor = [](const std::vector<std::string> &list, const std::string &key)
            -> std::optional<std::string> {
            const auto it = std::find(list.begin(), list.end(), key);
            if (it == list.end()) {
                return std::nullopt;
            }
            return it == list.begin() ? std::string() : *(it - 1);
        };
        std::set<std::string> keys(nowFirst.begin(), nowFirst.end());
        keys.insert(wasFirst.begin(), wasFirst.end());
        for (const auto &key : keys) {
            if (predecessor(nowFirst, key) != predecessor(wasFirst, key)
                && counts(domain::memberItemKey(id, key), stateIn(key))) {
                return true;
            }
        }
        return false;
    }
};

bool fallbackAddsSomething(const std::vector<Track> &rekordbox, const std::vector<Track> &engine,
                           const KeyFn &stickRelativeOf, const KeyFn &pathKeyOf)
{
    std::set<std::string> engineKeys;
    std::set<std::string> enginePaths;
    for (const Track &track : engine) {
        if (!track.streamingSource.empty()) {
            continue;
        }
        const std::string key = keyOf(track, stickRelativeOf, pathKeyOf);
        if (!key.empty()) {
            engineKeys.insert(key);
        }
        for (const auto &membership : track.playlists) {
            enginePaths.insert(membership.name);
        }
    }
    for (const Track &track : rekordbox) {
        if (!track.streamingSource.empty()) {
            continue;
        }
        const std::string key = keyOf(track, stickRelativeOf, pathKeyOf);
        if (!key.empty() && !engineKeys.count(key)) {
            return true;
        }
        for (const auto &membership : track.playlists) {
            if (!enginePaths.count(membership.name)) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

std::string engineUpdateNeedName(EngineUpdateNeed need)
{
    switch (need) {
    case EngineUpdateNeed::Cues:
        return "cues";
    case EngineUpdateNeed::Library:
        return "library";
    case EngineUpdateNeed::None:
        break;
    }
    return {};
}

EngineUpdateNeed summarizeEngineUpdate(const std::vector<domain::Track> &rekordboxTracksStage,
                                       const std::vector<domain::Track> &engineTracksStage,
                                       const std::optional<domain::RekordboxBaseline> &baseline,
                                       std::uint64_t pdbSequence, std::uint64_t engineCounter,
                                       const std::function<std::string(const std::string &)> &stickRelativeOf,
                                       const std::function<std::string(const std::string &)> &pathKeyOf)
{
    if (baseline) {
        if (WithBaseline(rekordboxTracksStage, *baseline, stickRelativeOf, pathKeyOf).libraryDiffers()) {
            return EngineUpdateNeed::Library;
        }
        return pdbSequence != baseline->pdbSequence ? EngineUpdateNeed::Cues : EngineUpdateNeed::None;
    }
    if (fallbackAddsSomething(rekordboxTracksStage, engineTracksStage, stickRelativeOf, pathKeyOf)) {
        return EngineUpdateNeed::Library;
    }
    return engineCounter != pdbSequence ? EngineUpdateNeed::Cues : EngineUpdateNeed::None;
}

}  // namespace seabass::application
