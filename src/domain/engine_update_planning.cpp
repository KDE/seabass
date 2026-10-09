// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/engine_update_planning.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <utility>

#include "domain/playlist_sync.hpp"
#include "domain/track_matching.hpp"

namespace seabass::domain
{

namespace
{

std::string parentPath(const std::string &path)
{
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string leafName(const std::string &path)
{
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::size_t depth(const std::string &path)
{
    return static_cast<std::size_t>(std::count(path.begin(), path.end(), '/'));
}

// Unrated and zero stars are one thing at the storage level (track.hpp).
std::optional<int> effectiveRating(std::optional<int> rating)
{
    return rating && *rating > 0 ? rating : std::nullopt;
}

std::string starsText(std::optional<int> rating)
{
    if (!rating) {
        return "no rating";
    }
    return std::to_string(*rating) + (*rating == 1 ? " star" : " stars");
}

std::string quoted(const std::string &text)
{
    return "\"" + text + "\"";
}

// First occurrence of each key, in order. A track listed twice in one
// playlist is planned by its first entry.
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

std::vector<std::string> keepOnly(const std::vector<std::string> &keys, const std::set<std::string> &allowed)
{
    std::vector<std::string> out;
    for (const auto &key : keys) {
        if (allowed.count(key)) {
            out.push_back(key);
        }
    }
    return out;
}

// The keys of a longest common subsequence of two lists of distinct keys:
// the members that keep their relative order. Everything else moved.
std::set<std::string> longestCommonOrder(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    const std::size_t n = a.size();
    const std::size_t m = b.size();
    std::vector<std::vector<std::size_t>> length(n + 1, std::vector<std::size_t>(m + 1, 0));
    for (std::size_t i = n; i-- > 0;) {
        for (std::size_t j = m; j-- > 0;) {
            length[i][j] = a[i] == b[j] ? length[i + 1][j + 1] + 1 : std::max(length[i + 1][j], length[i][j + 1]);
        }
    }
    std::set<std::string> out;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < n && j < m) {
        if (a[i] == b[j]) {
            out.insert(a[i]);
            ++i;
            ++j;
        } else if (length[i + 1][j] >= length[i][j + 1]) {
            ++i;
        } else {
            ++j;
        }
    }
    return out;
}

EngineUpdateItemHeader makeHeader(std::string key, bool checked, bool conflict, EngineUpdateReason reason,
                                  std::string text, const std::string &state,
                                  std::vector<std::string> dependsOn = {})
{
    EngineUpdateItemHeader header;
    header.key = std::move(key);
    header.checkedByDefault = checked;
    header.conflict = conflict;
    header.reason = reason;
    header.reasonText = std::move(text);
    header.dependsOn = std::move(dependsOn);
    header.rekordboxState = engineUpdateStateHash(state);
    return header;
}

// Calls `f` on every vector of rows that can be written or decided.
template<typename F>
void eachDecidable(EngineUpdateProposal &p, F &&f)
{
    f(p.playlistsToCreate);
    f(p.tracksToAdd);
    f(p.playlistsToRename);
    f(p.membership);
    f(p.metadataToEngine);
    f(p.cuesToEngine);
    f(p.playlistsToDelete);
    f(p.tracksToRemove);
    f(p.restoresToRekordbox);
    f(p.cuesToRekordbox);
    f(p.conflicts);
}

class Planner
{
public:
    explicit Planner(const EngineUpdateInput &input)
        : in(input)
        , base(input.baseline ? &*input.baseline : nullptr)
    {
    }

    EngineUpdateProposal run()
    {
        out.hasBaseline = base != nullptr;
        out.baselineSequence = base ? base->pdbSequence : 0;
        out.currentSequence = in.currentSequence;
        index();
        planTracks();
        planPlaylists();
        planMembership();
        planMetadata();
        suppressDeclined();
        return std::move(out);
    }

private:
    const EngineUpdateInput &in;
    const RekordboxBaseline *base;
    EngineUpdateProposal out;

    std::vector<Track> engine;  // without streaming rows
    RekordboxBaseline now;      // rekordbox now, in the baseline's shape
    std::map<std::string, const Track *> rByKey;
    std::map<std::string, std::vector<const Track *>> eByKey;
    std::set<std::string> duplicateKeys;
    std::set<std::string> addedKeys;
    // Engine playlist members by Engine path, in Engine's order.
    std::map<std::string, std::vector<std::string>> eMembers;
    std::map<std::string, int> eCount;

    struct PlaylistStatus
    {
        bool planMembers = false;
        bool created = false;
        std::string engineNow;  // Engine's path today
        // What a row inside this playlist depends on: its create or
        // rename and its parents'; a conflict's key when it is undecided.
        std::vector<std::string> deps;
    };
    std::map<std::uint32_t, PlaylistStatus> status;
    std::vector<std::pair<std::string, std::string>> renamesInOrder;
    std::set<std::string> claimedEnginePaths;

    std::string relativeOf(const std::string &filePath) const
    {
        return in.stickRelativeOf ? in.stickRelativeOf(filePath) : filePath;
    }

    std::string keyOf(const Track &track) const
    {
        const std::string relative = relativeOf(track.filePath);
        if (relative.empty()) {
            return {};
        }
        return in.pathKeyOf ? in.pathKeyOf(relative) : relative;
    }

    const Track *engineRow(const std::string &key) const
    {
        const auto it = eByKey.find(key);
        return it != eByKey.end() && it->second.size() == 1 ? it->second.front() : nullptr;
    }

    int countAt(const std::string &path) const
    {
        const auto it = eCount.find(path);
        return it == eCount.end() ? 0 : it->second;
    }

    std::string since() const
    {
        return " after Seabass last recorded the stick (export " + std::to_string(in.currentSequence)
            + ", recorded at " + std::to_string(base ? base->pdbSequence : 0) + ")";
    }

    std::string pathAfterRenames(std::string path) const
    {
        for (const auto &[from, to] : renamesInOrder) {
            if (path == from) {
                path = to;
            } else if (path.size() > from.size() && path.compare(0, from.size(), from) == 0 && path[from.size()] == '/') {
                path = to + path.substr(from.size());
            }
        }
        return path;
    }

    void index()
    {
        engine.reserve(in.engine.size());
        for (const auto &track : in.engine) {
            if (track.streamingSource.empty()) {
                engine.push_back(track);
            }
        }
        now = baselineFrom(
            in.rekordbox, in.rekordboxPlaylists, in.currentSequence,
            [&](const std::string &p) { return relativeOf(p); },
            [&](const std::string &p) { return in.pathKeyOf ? in.pathKeyOf(p) : p; });
        for (const auto &track : in.rekordbox) {
            const std::string key = keyOf(track);
            if (!key.empty()) {
                rByKey.emplace(key, &track);  // two rows for one file: the first, as the baseline does
            }
        }

        // Pairing: matchTracks within one stick, then the rekordbox row's
        // key, so a row paired by name still lands on its partner.
        std::map<const Track *, std::string> pairedKey;
        for (const auto &[r, e] : matchTracks(in.rekordbox, engine, MatchScope::OneStick)) {
            const std::string key = keyOf(*r);
            if (!key.empty()) {
                pairedKey.emplace(e, key);
            }
        }
        std::map<const Track *, std::string> keyOfEngineRow;
        for (const auto &track : engine) {
            const auto it = pairedKey.find(&track);
            const std::string key = it != pairedKey.end() ? it->second : keyOf(track);
            if (key.empty()) {
                continue;
            }
            eByKey[key].push_back(&track);
            keyOfEngineRow.emplace(&track, key);
        }
        for (const auto &[key, rows] : eByKey) {
            if (rows.size() > 1) {
                duplicateKeys.insert(key);
            }
        }

        // Two entries at one path are ambiguous whatever countAtPath says.
        for (const auto &playlist : in.enginePlaylists) {
            ++eCount[playlist.path];
        }
        for (const auto &playlist : in.enginePlaylists) {
            auto &count = eCount[playlist.path];
            count = std::max(count, playlist.countAtPath);
        }
        struct Entry
        {
            int position;
            std::size_t seen;
            std::string key;
        };
        std::map<std::string, std::vector<Entry>> entries;
        std::size_t seen = 0;
        for (const auto &track : engine) {
            const auto it = keyOfEngineRow.find(&track);
            if (it == keyOfEngineRow.end()) {
                continue;
            }
            for (const auto &membership : track.playlists) {
                entries[membership.name].push_back(Entry{membership.position, seen++, it->second});
            }
        }
        for (auto &[path, list] : entries) {
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
            auto &members = eMembers[path];
            for (const auto &entry : list) {
                members.push_back(entry.key);
            }
        }
    }

    // Tracks: presence of each file in R, E and B.
    void planTracks()
    {
        std::set<std::string> keys;
        for (const auto &[key, _] : rByKey) {
            keys.insert(key);
        }
        for (const auto &[key, _] : eByKey) {
            keys.insert(key);
        }
        const std::string engineName = catalogDisplayName("engine");
        for (const auto &key : keys) {
            const auto rIt = rByKey.find(key);
            const Track *r = rIt == rByKey.end() ? nullptr : rIt->second;
            const std::string state = r ? "track:present:" + relativeOf(r->filePath) : "track:absent";
            const std::string itemKeyText = trackItemKey(key);

            if (duplicateKeys.count(key)) {
                const auto &rows = eByKey.at(key);
                EngineUpdateConflict conflict;
                conflict.header = makeHeader(itemKeyText, false, true, EngineUpdateReason::DuplicateEngineRows,
                                             engineName + " lists this file " + std::to_string(rows.size())
                                                 + " times: Clean Up first",
                                             state);
                conflict.pathKey = key;
                conflict.rekordboxSide = r ? "rekordbox lists it once" : "rekordbox does not list it";
                conflict.engineSide = engineName + " lists it " + std::to_string(rows.size()) + " times";
                out.conflicts.push_back(std::move(conflict));
                continue;
            }
            const Track *e = engineRow(key);
            if (r && e) {
                continue;
            }
            const BaselineTrack *b = base ? base->findTrack(key) : nullptr;

            if (r) {
                if (base && b) {
                    EngineOwnItem kept;
                    kept.header = makeHeader(itemKeyText, false, false, EngineUpdateReason::EngineOwn,
                                             engineName
                                                 + " no longer lists this track; rekordbox still does, as it did when "
                                                   "Seabass last recorded the stick",
                                             state);
                    out.engineOwnKept.push_back(std::move(kept));
                    continue;
                }
                TrackToAdd add;
                add.rekordbox = *r;
                add.stickRelativePath = relativeOf(r->filePath);
                add.pathKey = key;
                if (in.fileExists && !in.fileExists(add.stickRelativePath)) {
                    add.header = makeHeader(itemKeyText, false, false, EngineUpdateReason::FileNotOnStick,
                                            "rekordbox lists this file and the stick does not have it: nothing to add",
                                            state);
                    out.notAdded.push_back(std::move(add));
                    continue;
                }
                if (base) {
                    add.header = makeHeader(itemKeyText, true, false, EngineUpdateReason::RekordboxAdded,
                                            "rekordbox added this track" + since(), state);
                } else {
                    add.header = makeHeader(itemKeyText, true, false, EngineUpdateReason::NoBaselineAddition,
                                            "No earlier record of this stick: rekordbox lists this track and "
                                                + engineName + " does not",
                                            state);
                }
                addedKeys.insert(key);
                out.tracksToAdd.push_back(std::move(add));
                continue;
            }

            // Engine lists it, rekordbox does not.
            TrackToRemove remove;
            remove.engine = *e;
            remove.pathKey = key;
            remove.playlistCount = static_cast<int>(e->playlists.size());
            if (base) {
                if (b) {
                    remove.header = makeHeader(itemKeyText, true, false, EngineUpdateReason::RekordboxRemoved,
                                               "rekordbox removed this track" + since(), state);
                    out.tracksToRemove.push_back(std::move(remove));
                } else {
                    EngineOwnItem kept;
                    kept.engine = *e;
                    kept.header = makeHeader(itemKeyText, false, false, EngineUpdateReason::EngineOwn,
                                             engineName + " added this track after Seabass last recorded the stick",
                                             state);
                    out.engineOwnKept.push_back(std::move(kept));
                }
                continue;
            }
            const auto importIt = in.enginePdbImportKey.find(e->sourceId);
            const bool imported = importIt == in.enginePdbImportKey.end() || importIt->second != 0;
            if (imported) {
                remove.header = makeHeader(itemKeyText, true, false, EngineUpdateReason::NoBaselineImportedRow,
                                           "Remove it from " + engineName, state);
                EngineUpdateConflict conflict;
                conflict.header = makeHeader(itemKeyText, false, true, EngineUpdateReason::NoBaselineImportedRow,
                                             engineName
                                                 + " imported this track from rekordbox, which no longer lists it; no "
                                                   "earlier record says whether it was removed there",
                                             state);
                conflict.pathKey = key;
                conflict.rekordboxSide = "Remove it from " + engineName;
                conflict.engineSide = "Keep it in " + engineName;
                conflict.rekordboxChoice.emplace_back(std::move(remove));
                out.conflicts.push_back(std::move(conflict));
            } else {
                EngineOwnItem kept;
                kept.engine = *e;
                kept.header = makeHeader(itemKeyText, false, false, EngineUpdateReason::NoBaselineEngineOnly,
                                         engineName + "'s own track: no rekordbox import put it there", state);
                out.engineOwnKept.push_back(std::move(kept));
            }
        }
    }

    static std::string playlistState(const BaselinePlaylist *r)
    {
        if (!r) {
            return "playlist:absent";
        }
        return std::string(r->folder ? "folder:" : "list:") + r->path;
    }

    void ambiguous(std::uint32_t id, const std::string &enginePath, const std::string &state)
    {
        const std::string engineName = catalogDisplayName("engine");
        EngineUpdateConflict conflict;
        conflict.header = makeHeader(playlistItemKey(id), false, true, EngineUpdateReason::EnginePathAmbiguous,
                                     engineName + " has " + std::to_string(countAt(enginePath)) + " playlists at "
                                         + quoted(enginePath) + ": rename one in " + engineName + " DJ first",
                                     state);
        conflict.rekordboxSide = "rekordbox has one";
        conflict.engineSide = engineName + " has " + std::to_string(countAt(enginePath));
        out.conflicts.push_back(std::move(conflict));
    }

    // An Engine-only playlist in the same folder holding nearly the same
    // tracks: maybe this one renamed. Exactly one candidate or none.
    std::optional<std::string> renameGuess(const BaselinePlaylist &r, const std::set<std::string> &rekordboxPaths) const
    {
        if (r.folder || r.members.empty()) {
            return std::nullopt;
        }
        const std::set<std::string> rSet(r.members.begin(), r.members.end());
        std::optional<std::string> found;
        int candidates = 0;
        for (const auto &playlist : in.enginePlaylists) {
            if (playlist.folder || rekordboxPaths.count(playlist.path) || claimedEnginePaths.count(playlist.path)
                || countAt(playlist.path) != 1 || parentPath(playlist.path) != parentPath(r.path)) {
                continue;
            }
            const auto it = eMembers.find(playlist.path);
            if (it == eMembers.end()) {
                continue;
            }
            const std::set<std::string> eSet(it->second.begin(), it->second.end());
            std::size_t common = 0;
            for (const auto &key : eSet) {
                common += rSet.count(key);
            }
            // Closely: at least four in five of the larger list.
            if (common > 0 && common * 5 >= std::max(rSet.size(), eSet.size()) * 4) {
                ++candidates;
                found = playlist.path;
            }
        }
        return candidates == 1 ? found : std::nullopt;
    }

    void planPlaylists()
    {
        const std::string engineName = catalogDisplayName("engine");
        std::vector<std::size_t> order(now.playlists.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return depth(now.playlists[a].path) < depth(now.playlists[b].path);
        });
        std::set<std::string> rekordboxPaths;
        std::set<std::uint32_t> rekordboxIds;
        for (const auto &playlist : now.playlists) {
            rekordboxPaths.insert(playlist.path);
            rekordboxIds.insert(playlist.id);
        }

        for (const std::size_t i : order) {
            const BaselinePlaylist &r = now.playlists[i];
            const std::string key = playlistItemKey(r.id);
            const std::string state = playlistState(&r);
            PlaylistStatus st;
            std::vector<std::string> parentDeps;
            if (r.parentId != 0) {
                const auto parent = status.find(r.parentId);
                if (parent != status.end()) {
                    parentDeps = parent->second.deps;
                }
            }
            st.deps = parentDeps;
            const BaselinePlaylist *b = base ? base->findPlaylist(r.id) : nullptr;

            if (countAt(r.path) > 1) {
                claimedEnginePaths.insert(r.path);
                ambiguous(r.id, r.path, state);
                status[r.id] = st;
                continue;
            }
            if (countAt(r.path) == 1) {
                claimedEnginePaths.insert(r.path);
                st.planMembers = true;
                st.engineNow = r.path;
                status[r.id] = st;
                continue;
            }
            if (b && b->path != r.path && countAt(b->path) > 1) {
                claimedEnginePaths.insert(b->path);
                ambiguous(r.id, b->path, state);
                status[r.id] = st;
                continue;
            }
            if (b && b->path != r.path && countAt(b->path) == 1) {
                claimedEnginePaths.insert(b->path);
                st.planMembers = true;
                st.engineNow = b->path;
                const bool ownChange = leafName(b->path) != leafName(r.path) || b->parentId != r.parentId;
                if (ownChange) {
                    PlaylistRename rename;
                    rename.pdbId = r.id;
                    rename.fromPath = pathAfterRenames(b->path);
                    rename.toPath = r.path;
                    rename.header = makeHeader(key, true, false, EngineUpdateReason::RekordboxRenamed,
                                               "rekordbox renamed this playlist from " + quoted(b->path) + " to "
                                                   + quoted(r.path) + since(),
                                               state, parentDeps);
                    renamesInOrder.emplace_back(rename.fromPath, rename.toPath);
                    out.playlistsToRename.push_back(std::move(rename));
                    st.deps.push_back(key);
                }
                // Otherwise it moves with a renamed parent and needs nothing.
                status[r.id] = st;
                continue;
            }

            PlaylistCreate create;
            create.pdbId = r.id;
            create.path = r.path;
            create.folder = r.folder;

            if (b) {
                // Engine has neither its old path nor its new one.
                if (b->path == r.path) {
                    EngineOwnItem kept;
                    kept.playlistPath = r.path;
                    kept.header = makeHeader(key, false, false, EngineUpdateReason::EngineOwn,
                                             engineName
                                                 + " deleted this playlist after Seabass last recorded the stick; "
                                                   "rekordbox still has it",
                                             state);
                    out.engineOwnKept.push_back(std::move(kept));
                    status[r.id] = st;
                    continue;
                }
                create.header = makeHeader(key, true, false, EngineUpdateReason::BothChanged,
                                           "Create " + quoted(r.path) + " in " + engineName, state, parentDeps);
                EngineUpdateConflict conflict;
                conflict.header = makeHeader(key, false, true, EngineUpdateReason::BothChanged,
                                             "rekordbox renamed this playlist to " + quoted(r.path) + " and "
                                                 + engineName + " no longer has " + quoted(b->path),
                                             state, parentDeps);
                conflict.rekordboxSide = "Create " + quoted(r.path) + " in " + engineName;
                conflict.engineSide = "Leave " + engineName + " without it";
                conflict.rekordboxChoice.emplace_back(std::move(create));
                out.conflicts.push_back(std::move(conflict));
                st.deps = {key};
                status[r.id] = st;
                continue;
            }

            if (!base) {
                if (const auto guess = renameGuess(r, rekordboxPaths)) {
                    claimedEnginePaths.insert(*guess);
                    const auto &eSet = eMembers.at(*guess);
                    const std::set<std::string> eKeys(eSet.begin(), eSet.end());
                    std::size_t common = 0;
                    for (const auto &member : std::set<std::string>(r.members.begin(), r.members.end())) {
                        common += eKeys.count(member);
                    }
                    PlaylistRename rename;
                    rename.pdbId = r.id;
                    rename.fromPath = pathAfterRenames(*guess);
                    rename.toPath = r.path;
                    rename.header = makeHeader(key, true, false, EngineUpdateReason::NoBaselineRenameGuess,
                                               "Rename " + quoted(*guess) + " to " + quoted(r.path), state, parentDeps);
                    create.header = makeHeader(key, true, false, EngineUpdateReason::NoBaselineRenameGuess,
                                               "Create " + quoted(r.path) + " beside " + quoted(*guess), state,
                                               parentDeps);
                    EngineUpdateConflict conflict;
                    conflict.header = makeHeader(
                        key, false, true, EngineUpdateReason::NoBaselineRenameGuess,
                        "No earlier record of this stick: " + engineName + "'s " + quoted(*guess)
                            + " sits in the same folder and holds " + std::to_string(common) + " of these "
                            + std::to_string(std::set<std::string>(r.members.begin(), r.members.end()).size())
                            + " tracks; rekordbox may have renamed it",
                        state, parentDeps);
                    conflict.rekordboxSide = "Rename " + engineName + "'s " + quoted(*guess) + " to " + quoted(r.path);
                    conflict.engineSide = "Keep " + quoted(*guess) + " and create " + quoted(r.path);
                    conflict.rekordboxChoice.emplace_back(std::move(rename));
                    conflict.engineChoice.emplace_back(std::move(create));
                    out.conflicts.push_back(std::move(conflict));
                    // Its members follow once the choice is saved and the
                    // baseline knows the playlist.
                    st.deps = {key};
                    status[r.id] = st;
                    continue;
                }
            }

            const std::string parent = parentPath(r.path);
            if (!parent.empty() && countAt(parent) > 1) {
                claimedEnginePaths.insert(parent);
                ambiguous(r.id, parent, state);
                status[r.id] = st;
                continue;
            }
            if (base) {
                create.header = makeHeader(key, true, false, EngineUpdateReason::RekordboxAdded,
                                           std::string("rekordbox created this ") + (r.folder ? "folder" : "playlist")
                                               + since(),
                                           state, parentDeps);
            } else {
                create.header = makeHeader(key, true, false, EngineUpdateReason::NoBaselineAddition,
                                           "No earlier record of this stick: rekordbox has this "
                                               + std::string(r.folder ? "folder" : "playlist") + " and " + engineName
                                               + " does not",
                                           state, parentDeps);
            }
            out.playlistsToCreate.push_back(std::move(create));
            st.planMembers = true;
            st.created = true;
            st.deps.push_back(key);
            status[r.id] = st;
        }

        // Deletes: in B, gone from rekordbox, still in Engine. Deepest
        // first, so a folder's playlists go before the folder.
        if (base) {
            std::vector<const BaselinePlaylist *> gone;
            for (const auto &b : base->playlists) {
                if (!rekordboxIds.count(b.id) && !rekordboxPaths.count(b.path) && countAt(b.path) > 0) {
                    gone.push_back(&b);
                }
            }
            std::stable_sort(gone.begin(), gone.end(), [](const BaselinePlaylist *a, const BaselinePlaylist *b) {
                return depth(a->path) > depth(b->path);
            });
            for (const BaselinePlaylist *b : gone) {
                const std::string key = playlistItemKey(b->id);
                const std::string state = playlistState(nullptr);
                claimedEnginePaths.insert(b->path);
                if (countAt(b->path) > 1) {
                    ambiguous(b->id, b->path, state);
                    continue;
                }
                PlaylistDelete del;
                del.pdbId = b->id;
                del.path = pathAfterRenames(b->path);
                del.folder = b->folder;
                const auto members = eMembers.find(b->path);
                del.engineMembers = members == eMembers.end() ? 0 : static_cast<int>(members->second.size());
                const std::set<std::string> engineSet = members == eMembers.end()
                    ? std::set<std::string>()
                    : std::set<std::string>(members->second.begin(), members->second.end());
                const std::set<std::string> baseSet(b->members.begin(), b->members.end());
                if (!b->folder && engineSet != baseSet) {
                    del.header = makeHeader(key, true, false, EngineUpdateReason::BothChanged,
                                            "Delete " + quoted(b->path) + " from " + engineName, state);
                    EngineUpdateConflict conflict;
                    conflict.header = makeHeader(key, false, true, EngineUpdateReason::BothChanged,
                                                 "rekordbox deleted this playlist and " + engineName
                                                     + "'s copy changed since Seabass last recorded the stick",
                                                 state);
                    conflict.rekordboxSide = "Delete " + quoted(b->path) + " from " + engineName;
                    conflict.engineSide = "Keep it in " + engineName;
                    conflict.rekordboxChoice.emplace_back(std::move(del));
                    out.conflicts.push_back(std::move(conflict));
                    continue;
                }
                del.header = makeHeader(key, true, false, EngineUpdateReason::RekordboxRemoved,
                                        std::string("rekordbox deleted this ") + (b->folder ? "folder" : "playlist")
                                            + since(),
                                        state);
                out.playlistsToDelete.push_back(std::move(del));
            }
        }

        // Engine's own playlists: no rekordbox playlist accounts for them.
        std::set<std::string> listed;
        for (const auto &playlist : in.enginePlaylists) {
            if (claimedEnginePaths.count(playlist.path) || !listed.insert(playlist.path).second) {
                continue;
            }
            EngineOwnItem kept;
            kept.playlistPath = playlist.path;
            // No key: rekordbox has no id for a playlist it never had.
            kept.header = makeHeader({}, false, false,
                                     base ? EngineUpdateReason::EngineOwn : EngineUpdateReason::NoBaselineEngineOnly,
                                     engineName + "'s own " + (playlist.folder ? "folder" : "playlist")
                                         + ": rekordbox has nothing at this path",
                                     "playlist:absent");
            out.engineOwnKept.push_back(std::move(kept));
        }
    }

    // Membership: a three-way merge of each playlist's member set, then of
    // the order of the members both sides keep.
    void planMembership()
    {
        const std::string engineName = catalogDisplayName("engine");
        for (const auto &r : now.playlists) {
            const auto stIt = status.find(r.id);
            if (r.folder || stIt == status.end() || !stIt->second.planMembers) {
                continue;
            }
            const PlaylistStatus &st = stIt->second;
            const std::string &path = r.path;
            const std::vector<std::string> rm = firstOccurrences(r.members);
            std::vector<std::string> em;
            if (!st.created) {
                const auto it = eMembers.find(st.engineNow);
                if (it != eMembers.end()) {
                    em = firstOccurrences(it->second);
                }
            }
            const BaselinePlaylist *bp = base ? base->findPlaylist(r.id) : nullptr;
            const std::set<std::string> rSet(rm.begin(), rm.end());
            const std::set<std::string> eSet(em.begin(), em.end());
            std::set<std::string> bSet;
            if (bp) {
                bSet.insert(bp->members.begin(), bp->members.end());
            }

            // rekordbox's state of a member: in or out, and after which.
            const auto memberState = [&](const std::string &key) {
                const auto it = std::find(r.members.begin(), r.members.end(), key);
                if (it == r.members.end()) {
                    return std::string("member:out");
                }
                return "member:in:after:" + (it == r.members.begin() ? std::string() : *(it - 1));
            };
            const auto eligible = [&](const std::string &key) {
                return rByKey.count(key) && !duplicateKeys.count(key);
            };
            const auto removeEdit = [&](const std::string &key, const std::string &headerKey) {
                MembershipEdit edit;
                edit.kind = MembershipEdit::Kind::Remove;
                edit.pdbId = r.id;
                edit.playlistPath = path;
                edit.pathKey = key;
                if (const Track *row = engineRow(key)) {
                    edit.track = *row;
                }
                edit.header.key = headerKey;
                edit.header.dependsOn = st.deps;
                return edit;
            };
            const auto addEdit = [&](const std::string &key, const std::string &after) {
                MembershipEdit edit;
                edit.kind = MembershipEdit::Kind::Add;
                edit.pdbId = r.id;
                edit.playlistPath = path;
                edit.pathKey = key;
                edit.track = *rByKey.at(key);
                edit.afterPathKey = after;
                edit.header.key = memberItemKey(r.id, key);
                edit.header.dependsOn = st.deps;
                if (addedKeys.count(key)) {
                    edit.header.dependsOn.push_back(trackItemKey(key));
                }
                return edit;
            };
            const auto finish = [&](MembershipEdit &edit, bool checked, EngineUpdateReason reason, std::string text,
                                    const std::string &stateKey) {
                auto deps = std::move(edit.header.dependsOn);
                edit.header = makeHeader(edit.header.key, checked, false, reason, std::move(text),
                                         memberState(stateKey), std::move(deps));
            };

            std::vector<MembershipEdit> removes;
            std::vector<MembershipEdit> adds;

            // Members only Engine's copy holds, of tracks rekordbox lists.
            for (const auto &key : em) {
                if (!eligible(key) || rSet.count(key)) {
                    continue;
                }
                const std::string itemKeyText = memberItemKey(r.id, key);
                if (bp && bSet.count(key)) {
                    auto edit = removeEdit(key, itemKeyText);
                    finish(edit, true, EngineUpdateReason::RekordboxRemoved,
                           "rekordbox took this track out of " + quoted(path) + since(), key);
                    removes.push_back(std::move(edit));
                } else if (bp) {
                    EngineOwnItem kept;
                    if (const Track *row = engineRow(key)) {
                        kept.engine = *row;
                    }
                    kept.playlistPath = path;
                    kept.header = makeHeader(itemKeyText, false, false, EngineUpdateReason::EngineOwn,
                                             engineName + " added this track to " + quoted(path)
                                                 + " after Seabass last recorded the stick",
                                             memberState(key));
                    out.engineOwnKept.push_back(std::move(kept));
                } else {
                    auto edit = removeEdit(key, itemKeyText);
                    finish(edit, true, EngineUpdateReason::NoBaselineEngineMember,
                           "Take it out of " + engineName + "'s " + quoted(path), key);
                    EngineUpdateConflict conflict;
                    conflict.header = makeHeader(itemKeyText, false, true, EngineUpdateReason::NoBaselineEngineMember,
                                                 engineName + "'s " + quoted(path)
                                                     + " holds this track and rekordbox's does not; no earlier "
                                                       "record says which side changed it",
                                                 memberState(key), st.deps);
                    conflict.pathKey = key;
                    conflict.rekordboxSide = "Take it out of " + engineName + "'s " + quoted(path);
                    conflict.engineSide = "Keep it there";
                    conflict.rekordboxChoice.emplace_back(std::move(edit));
                    out.conflicts.push_back(std::move(conflict));
                }
            }

            // Members only rekordbox's copy holds.
            std::set<std::string> addSet;
            for (const auto &key : rm) {
                if (!eligible(key) || eSet.count(key)) {
                    continue;
                }
                // A track Engine neither lists nor gets: its own row says why.
                if (!engineRow(key) && !addedKeys.count(key)) {
                    continue;
                }
                if (bp && bSet.count(key)) {
                    EngineOwnItem kept;
                    if (const Track *row = engineRow(key)) {
                        kept.engine = *row;
                    }
                    kept.playlistPath = path;
                    kept.header = makeHeader(memberItemKey(r.id, key), false, false, EngineUpdateReason::EngineOwn,
                                             engineName + " took this track out of " + quoted(path)
                                                 + " after Seabass last recorded the stick",
                                             memberState(key));
                    out.engineOwnKept.push_back(std::move(kept));
                    continue;
                }
                addSet.insert(key);
            }

            // The order of the members both keep.
            std::set<std::string> common;
            for (const auto &key : rm) {
                if (eSet.count(key) && !duplicateKeys.count(key)) {
                    common.insert(key);
                }
            }
            const std::vector<std::string> rc = keepOnly(rm, common);
            const std::vector<std::string> ec = keepOnly(em, common);
            std::set<std::string> moved;
            enum class OrderMode { Apply, Conflict } orderMode = OrderMode::Apply;
            EngineUpdateReason orderReason = EngineUpdateReason::RekordboxMoved;
            if (rc != ec) {
                const std::set<std::string> kept = longestCommonOrder(ec, rc);
                for (const auto &key : rc) {
                    if (!kept.count(key)) {
                        moved.insert(key);
                    }
                }
                if (bp) {
                    std::set<std::string> inAll;
                    for (const auto &key : common) {
                        if (bSet.count(key)) {
                            inAll.insert(key);
                        }
                    }
                    const auto rb = keepOnly(rc, inAll);
                    const auto eb = keepOnly(ec, inAll);
                    const auto bb = keepOnly(firstOccurrences(bp->members), inAll);
                    if (eb == bb && rb != bb) {
                        orderMode = OrderMode::Apply;
                    } else if (rb == bb && eb != bb) {
                        EngineOwnItem keptRow;
                        keptRow.playlistPath = path;
                        keptRow.header = makeHeader(playlistItemKey(r.id), false, false, EngineUpdateReason::EngineOwn,
                                                    engineName + " changed the order of " + quoted(path)
                                                        + " after Seabass last recorded the stick",
                                                    playlistState(&r));
                        out.engineOwnKept.push_back(std::move(keptRow));
                        moved.clear();
                    } else {
                        orderMode = OrderMode::Conflict;
                        orderReason = EngineUpdateReason::BothChanged;
                    }
                } else {
                    orderMode = OrderMode::Conflict;
                    orderReason = EngineUpdateReason::NoBaselineOrder;
                }
            }

            // A copy of the same song Engine's playlist holds in place of
            // one rekordbox's adds, of a file rekordbox does not list at
            // all: it goes out as the other goes in (alignTo's swap).
            std::set<std::string> swapped;
            for (const auto &key : rm) {
                if (!addSet.count(key)) {
                    continue;
                }
                for (const auto &other : em) {
                    const Track *row = engineRow(other);
                    if (!row || rByKey.count(other) || swapped.count(other) || !sameSong(*row, *rByKey.at(key))) {
                        continue;
                    }
                    swapped.insert(other);
                    auto edit = removeEdit(other, memberItemKey(r.id, key));
                    if (addedKeys.count(key)) {
                        edit.header.dependsOn.push_back(trackItemKey(key));
                    }
                    finish(edit, true, EngineUpdateReason::SameSongCopy,
                           "Another copy of this song in " + engineName + "'s " + quoted(path)
                               + " makes room for rekordbox's",
                           key);
                    removes.push_back(std::move(edit));
                    break;
                }
            }

            // Adds and moves in rekordbox's order, each after the member
            // before it that Engine's copy will hold.
            std::string anchor;
            for (const auto &key : rm) {
                const bool isMove = moved.count(key) > 0;
                if (isMove || addSet.count(key)) {
                    auto add = addEdit(key, anchor);
                    if (!isMove) {
                        const bool known = bp || (base && st.created);
                        finish(add, true, known ? EngineUpdateReason::RekordboxAdded : EngineUpdateReason::NoBaselineAddition,
                               known ? "rekordbox added this track to " + quoted(path) + since()
                                     : "No earlier record of this playlist: rekordbox's " + quoted(path)
                                         + " holds this track and " + engineName + "'s does not",
                               key);
                        adds.push_back(std::move(add));
                    } else {
                        auto remove = removeEdit(key, memberItemKey(r.id, key));
                        if (orderMode == OrderMode::Apply) {
                            const std::string text = "rekordbox moved this track within " + quoted(path) + since();
                            finish(remove, true, orderReason, text, key);
                            finish(add, true, orderReason, text, key);
                            removes.push_back(std::move(remove));
                            adds.push_back(std::move(add));
                        } else {
                            const std::string text = "Move it to rekordbox's place";
                            finish(remove, true, orderReason, text, key);
                            finish(add, true, orderReason, text, key);
                            EngineUpdateConflict conflict;
                            conflict.header = makeHeader(
                                memberItemKey(r.id, key), false, true, orderReason,
                                orderReason == EngineUpdateReason::BothChanged
                                    ? "Both sides changed the order of " + quoted(path)
                                        + " since Seabass last recorded the stick"
                                    : "No earlier record of this stick: this track sits elsewhere in " + engineName
                                        + "'s " + quoted(path) + " than in rekordbox's",
                                memberState(key), st.deps);
                            conflict.pathKey = key;
                            conflict.rekordboxSide = "Move it to rekordbox's place";
                            conflict.engineSide = "Leave it where " + engineName + " has it";
                            conflict.rekordboxChoice.emplace_back(std::move(remove));
                            conflict.rekordboxChoice.emplace_back(std::move(add));
                            out.conflicts.push_back(std::move(conflict));
                        }
                    }
                    anchor = key;
                } else if (eSet.count(key)) {
                    anchor = key;
                }
            }
            for (auto &edit : removes) {
                out.membership.push_back(std::move(edit));
            }
            for (auto &edit : adds) {
                out.membership.push_back(std::move(edit));
            }
        }
    }

    void planMetadata()
    {
        const std::string engineName = catalogDisplayName("engine");
        for (const auto &[key, r] : rByKey) {
            const Track *e = engineRow(key);
            if (!e || duplicateKeys.count(key)) {
                continue;
            }
            const BaselineTrack *b = base ? base->findTrack(key) : nullptr;
            const auto edit = [&](MetadataEdit::Field field, MetadataEdit::Direction direction) {
                MetadataEdit m;
                m.field = field;
                m.direction = direction;
                m.rekordbox = *r;
                m.engine = *e;
                m.pathKey = key;
                return m;
            };

            // Rating.
            const auto rv = effectiveRating(r->rating);
            const auto ev = effectiveRating(e->rating);
            if (rv != ev) {
                const std::string itemKeyText = ratingItemKey(key);
                const std::string state = "rating:" + (rv ? std::to_string(*rv) : std::string("none"));
                const auto toEngine = [&](std::optional<int> value, bool checked, EngineUpdateReason reason,
                                          std::string text) {
                    auto m = edit(MetadataEdit::Field::Rating, MetadataEdit::Direction::ToEngine);
                    m.rating = value;
                    m.header = makeHeader(itemKeyText, checked, false, reason, std::move(text), state);
                    return m;
                };
                const auto toRekordbox = [&](std::optional<int> value, bool checked, EngineUpdateReason reason,
                                             std::string text) {
                    auto m = edit(MetadataEdit::Field::Rating, MetadataEdit::Direction::ToRekordbox);
                    m.rating = value;
                    m.header = makeHeader(itemKeyText, checked, false, reason, std::move(text), state);
                    return m;
                };
                const auto conflictOf = [&](EngineUpdateReason reason, std::string text) {
                    EngineUpdateConflict c;
                    c.header = makeHeader(itemKeyText, false, true, reason, std::move(text), state);
                    c.pathKey = key;
                    c.rekordboxSide = "rekordbox: " + starsText(rv);
                    c.engineSide = engineName + ": " + starsText(ev);
                    return c;
                };
                if (b) {
                    const auto bv = effectiveRating(b->rating);
                    if (bv == ev && !rv) {
                        if (b->ratingOrigin == ValueOrigin::Seabass) {
                            out.restoresToRekordbox.push_back(
                                toRekordbox(bv, true, EngineUpdateReason::ExportDropped,
                                            "rekordbox's export dropped the rating Seabass had written ("
                                                + starsText(bv) + "); it goes back"));
                        } else if (b->ratingOrigin == ValueOrigin::Rekordbox) {
                            out.metadataToEngine.push_back(
                                toEngine(rv, true, EngineUpdateReason::RekordboxChanged,
                                         "rekordbox cleared the rating (" + starsText(bv) + ")" + since()));
                        } else {
                            auto c = conflictOf(EngineUpdateReason::OriginUnknown,
                                                "rekordbox no longer has the rating " + engineName + " has ("
                                                    + starsText(ev)
                                                    + "), and nothing recorded whether Seabass or rekordbox wrote it");
                            c.rekordboxChoice.emplace_back(toEngine(rv, true, EngineUpdateReason::OriginUnknown,
                                                                    "Clear " + engineName + "'s rating"));
                            c.engineChoice.emplace_back(toRekordbox(ev, true, EngineUpdateReason::OriginUnknown,
                                                                    "Put " + starsText(ev) + " back on rekordbox"));
                            out.conflicts.push_back(std::move(c));
                        }
                    } else if (bv == ev) {
                        out.metadataToEngine.push_back(toEngine(rv, true, EngineUpdateReason::RekordboxChanged,
                                                                "rekordbox changed the rating from " + starsText(bv)
                                                                    + " to " + starsText(rv) + since()));
                    } else if (bv == rv) {
                        EngineOwnItem kept;
                        kept.engine = *e;
                        kept.header = makeHeader(itemKeyText, false, false, EngineUpdateReason::EngineOwn,
                                                 engineName + " changed the rating from " + starsText(bv) + " to "
                                                     + starsText(ev) + " after Seabass last recorded the stick",
                                                 state);
                        out.engineOwnKept.push_back(std::move(kept));
                    } else {
                        auto c = conflictOf(EngineUpdateReason::BothChanged,
                                            "Both sides changed the rating since Seabass last recorded the stick "
                                            "(rekordbox "
                                                + starsText(rv) + ", " + engineName + " " + starsText(ev)
                                                + ", recorded " + starsText(bv) + ")");
                        c.rekordboxChoice.emplace_back(
                            toEngine(rv, true, EngineUpdateReason::BothChanged, "Use rekordbox's rating"));
                        out.conflicts.push_back(std::move(c));
                    }
                } else {
                    auto c = conflictOf(EngineUpdateReason::NoBaselineValuesDiffer,
                                        "Ratings differ (rekordbox " + starsText(rv) + ", " + engineName + " "
                                            + starsText(ev) + ") and no earlier record says which side changed");
                    c.rekordboxChoice.emplace_back(
                        toEngine(rv, true, EngineUpdateReason::NoBaselineValuesDiffer, "Use rekordbox's rating"));
                    out.conflicts.push_back(std::move(c));
                }
            }

            // Comment. No origin is recorded for comments, so a comment
            // rekordbox lacks is never assumed dropped or deleted.
            if (r->comment != e->comment) {
                const std::string itemKeyText = commentItemKey(key);
                const std::string state = "comment:" + r->comment;
                const auto toEngine = [&](bool checked, EngineUpdateReason reason, std::string text) {
                    auto m = edit(MetadataEdit::Field::Comment, MetadataEdit::Direction::ToEngine);
                    m.comment = r->comment;
                    m.header = makeHeader(itemKeyText, checked, false, reason, std::move(text), state);
                    return m;
                };
                const auto conflictOf = [&](EngineUpdateReason reason, std::string text) {
                    EngineUpdateConflict c;
                    c.header = makeHeader(itemKeyText, false, true, reason, std::move(text), state);
                    c.pathKey = key;
                    c.rekordboxSide = r->comment.empty() ? "rekordbox: no comment" : "rekordbox: " + quoted(r->comment);
                    c.engineSide = e->comment.empty() ? engineName + ": no comment" : engineName + ": " + quoted(e->comment);
                    c.rekordboxChoice.emplace_back(toEngine(true, reason, "Use rekordbox's comment"));
                    return c;
                };
                if (b && b->comment == e->comment && !r->comment.empty()) {
                    out.metadataToEngine.push_back(
                        toEngine(true, EngineUpdateReason::RekordboxChanged, "rekordbox changed the comment" + since()));
                } else if (b && b->comment == e->comment) {
                    out.conflicts.push_back(conflictOf(EngineUpdateReason::OriginUnknown,
                                                       "rekordbox no longer has the comment " + engineName
                                                           + " has, and nothing recorded who wrote it"));
                } else if (b && b->comment == r->comment) {
                    EngineOwnItem kept;
                    kept.engine = *e;
                    kept.header = makeHeader(itemKeyText, false, false, EngineUpdateReason::EngineOwn,
                                             engineName + " changed the comment after Seabass last recorded the stick",
                                             state);
                    out.engineOwnKept.push_back(std::move(kept));
                } else if (b) {
                    out.conflicts.push_back(conflictOf(EngineUpdateReason::BothChanged,
                                                       "Both sides changed the comment since Seabass last recorded "
                                                       "the stick"));
                } else {
                    out.conflicts.push_back(conflictOf(EngineUpdateReason::NoBaselineValuesDiffer,
                                                       "Comments differ and no earlier record says which side "
                                                       "changed"));
                }
            }
        }
    }

    // Declined items stay out while rekordbox's state of them is the one
    // recorded; a row depending on one that stays out stays out too.
    void suppressDeclined()
    {
        if (!base || base->declined.empty()) {
            return;
        }
        std::set<std::string> suppressed;
        eachDecidable(out, [&](auto &rows) {
            for (const auto &row : rows) {
                const auto it = base->declined.find(row.header.key);
                if (it != base->declined.end() && it->second == row.header.rekordboxState) {
                    suppressed.insert(row.header.key);
                }
            }
        });
        if (suppressed.empty()) {
            return;
        }
        bool grew = true;
        while (grew) {
            grew = false;
            eachDecidable(out, [&](auto &rows) {
                for (const auto &row : rows) {
                    if (suppressed.count(row.header.key)) {
                        continue;
                    }
                    for (const auto &dep : row.header.dependsOn) {
                        if (suppressed.count(dep)) {
                            suppressed.insert(row.header.key);
                            grew = true;
                            break;
                        }
                    }
                }
            });
        }
        eachDecidable(out, [&](auto &rows) {
            rows.erase(std::remove_if(rows.begin(), rows.end(),
                                      [&](const auto &row) { return suppressed.count(row.header.key) > 0; }),
                       rows.end());
        });
        out.declinedSuppressed.assign(suppressed.begin(), suppressed.end());
    }
};

}  // namespace

bool EngineUpdateProposal::empty() const
{
    return playlistsToCreate.empty() && tracksToAdd.empty() && playlistsToRename.empty() && membership.empty()
        && metadataToEngine.empty() && cuesToEngine.empty() && playlistsToDelete.empty() && tracksToRemove.empty()
        && restoresToRekordbox.empty() && cuesToRekordbox.empty() && conflicts.empty();
}

bool EngineUpdateProposal::onlyCues() const
{
    if (empty()) {
        return false;
    }
    const bool cueConflictsOnly = std::all_of(conflicts.begin(), conflicts.end(), [](const EngineUpdateConflict &c) {
        return c.header.key.rfind("cue:", 0) == 0;
    });
    return cueConflictsOnly && playlistsToCreate.empty() && tracksToAdd.empty() && playlistsToRename.empty()
        && membership.empty() && metadataToEngine.empty() && playlistsToDelete.empty() && tracksToRemove.empty()
        && restoresToRekordbox.empty();
}

EngineUpdateProposal EngineUpdatePlanner::plan(const EngineUpdateInput &input)
{
    return Planner(input).run();
}

std::string engineUpdateStateHash(std::string_view canonicalState)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char c : canonicalState) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

}  // namespace seabass::domain
