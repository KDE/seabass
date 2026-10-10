// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/engine_update_planning.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <utility>

#include "domain/engine_cue_translation.hpp"
#include "domain/junk_cue.hpp"
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

// A value for a button: quoted, and cut after 32 bytes (never inside a
// UTF-8 sequence) so a long comment does not make a long button. The row's
// details give it in full.
std::string shortQuoted(const std::string &text)
{
    constexpr std::size_t Limit = 32;
    if (text.size() <= Limit) {
        return quoted(text);
    }
    std::size_t cut = Limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return quoted(text.substr(0, cut) + "...");
}

bool isHotCue(const CuePoint &cue)
{
    return cue.kind == CuePoint::Kind::Hot;
}

std::string wholeMs(double ms)
{
    return std::to_string(std::llround(ms));
}

// The canonical spelling of one cue for a state hash: kind, place and a
// loop's end, rounded to the millisecond. Colour and comment never take
// part (colour is carried, never compared).
std::string spellCue(const CuePoint &cue)
{
    return cue.isLoop ? "loop@" + wholeMs(cue.positionMs) + "-" + wholeMs(cue.loopEndMs)
                      : "cue@" + wholeMs(cue.positionMs);
}

std::vector<CuePoint> byPosition(std::vector<CuePoint> cues)
{
    std::stable_sort(cues.begin(), cues.end(),
                     [](const CuePoint &a, const CuePoint &b) { return a.positionMs < b.positionMs; });
    return cues;
}

// rekordbox's state of a pad: what it holds there, or none.
std::string padState(int pad, const std::vector<CuePoint> &cues)
{
    std::string state = "cue:hot:" + std::to_string(pad) + ":";
    if (cues.empty()) {
        return state + "none";
    }
    bool first = true;
    for (const CuePoint &cue : byPosition(cues)) {
        state += (first ? "" : ",") + spellCue(cue);
        first = false;
    }
    return state;
}

// rekordbox's state of a memory cue's place: the cue there, or none.
std::string memoryState(const CuePoint *cue)
{
    return cue ? std::string("cue:") + (cue->isLoop ? "loop:" : "memory:") + spellCue(*cue) : "cue:memory:none";
}

CuePoint padCue(int pad)
{
    CuePoint cue;
    cue.kind = CuePoint::Kind::Hot;
    cue.hotCueNumber = pad;
    return cue;
}

// "1:00.000", "loop 1:00.000", several joined; "empty" for none.
std::string places(const std::vector<CuePoint> &cues)
{
    if (cues.empty()) {
        return "empty";
    }
    std::string text;
    for (const CuePoint &cue : byPosition(cues)) {
        text += (text.empty() ? "" : ", ") + std::string(cue.isLoop ? "loop " : "") + formatCuePosition(cue.positionMs);
    }
    return text;
}

std::string memoryName(const CuePoint &cue)
{
    return std::string(cue.isLoop ? "the memory loop at " : "the memory cue at ") + formatCuePosition(cue.positionMs);
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
        planCues();
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
    // Cue items left out of their rows because the user declined them.
    std::set<std::string> cueSuppressed;

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
                // A refusal: no buttons, the reason says what to do.
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
                conflict.rekordboxSide = "Remove this track from " + engineName;
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
        // A refusal: no buttons, the reason says what to do.
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

    // rekordbox's playlists that share one path (the header's rule): the
    // group in id order, `paired` the one the baseline knew at this path
    // and planned as any other, or nullptr. One conflict for the rest.
    void sharedPath(const std::vector<const BaselinePlaylist *> &group, const BaselinePlaylist *paired,
                    const std::vector<std::string> &parentDeps)
    {
        const std::string engineName = catalogDisplayName("engine");
        const std::string &path = group.front()->path;
        std::vector<const BaselinePlaylist *> rest;
        for (const BaselinePlaylist *p : group) {
            if (p != paired) {
                rest.push_back(p);
            }
        }
        const BaselinePlaylist &first = *rest.front();
        const std::string key = playlistItemKey(first.id);
        // Every list of the group but the paired one, by id: the conflict
        // comes back when any of them changes.
        std::string state = "shared:" + path;
        for (const BaselinePlaylist *p : rest) {
            state += "\n" + std::to_string(p->id) + (p->folder ? ":folder" : ":list");
            for (const auto &member : p->members) {
                state += "\n" + member;
            }
        }
        const auto settle = [&](const std::vector<std::string> &deps) {
            for (const BaselinePlaylist *p : rest) {
                PlaylistStatus st;
                st.deps = deps;
                status[p->id] = st;
            }
        };
        const std::string parent = parentPath(path);
        if (countAt(path) > 1 || (!parent.empty() && countAt(parent) > 1)) {
            const std::string at = countAt(path) > 1 ? path : parent;
            claimedEnginePaths.insert(at);
            ambiguous(first.id, at, state);
            settle(parentDeps);
            return;
        }
        // A member renamed onto this path: Engine's playlist at its old
        // path is accounted for by this conflict, not Engine's own.
        if (base) {
            for (const BaselinePlaylist *p : rest) {
                const BaselinePlaylist *b = base->findPlaylist(p->id);
                if (b && b->path != path && countAt(b->path) == 1) {
                    claimedEnginePaths.insert(b->path);
                }
            }
        }

        const bool engineHas = countAt(path) == 1;
        // What Engine's playlist at the path holds, or will once the
        // paired playlist's own rows are applied.
        std::set<std::string> holds;
        if (engineHas) {
            claimedEnginePaths.insert(path);
            if (const auto it = eMembers.find(path); it != eMembers.end()) {
                holds.insert(it->second.begin(), it->second.end());
            }
            if (paired) {
                holds.insert(paired->members.begin(), paired->members.end());
            }
        }
        // The union in rekordbox order, first list first.
        std::vector<std::string> sequence;
        if (paired) {
            sequence = paired->members;
        }
        for (const BaselinePlaylist *p : rest) {
            sequence.insert(sequence.end(), p->members.begin(), p->members.end());
        }
        sequence = firstOccurrences(sequence);

        const std::string count = std::to_string(group.size());
        std::vector<EngineUpdateEdit> choice;
        if (!engineHas) {
            PlaylistCreate create;
            create.pdbId = first.id;
            create.path = path;
            create.header = makeHeader(key, true, false, EngineUpdateReason::RekordboxPathShared,
                                       "Create " + quoted(path) + " in " + engineName + " for all " + count, state,
                                       parentDeps);
            choice.emplace_back(std::move(create));
        }
        std::string anchor;
        for (const auto &member : sequence) {
            if (holds.count(member)) {
                anchor = member;
                continue;
            }
            if (!rByKey.count(member) || duplicateKeys.count(member) || (!engineRow(member) && !addedKeys.count(member))) {
                continue;
            }
            MembershipEdit add;
            add.kind = MembershipEdit::Kind::Add;
            add.pdbId = first.id;
            add.playlistPath = path;
            add.pathKey = member;
            add.track = *rByKey.at(member);
            add.afterPathKey = anchor;
            std::vector<std::string> deps = parentDeps;
            if (addedKeys.count(member)) {
                deps.push_back(trackItemKey(member));
            }
            add.header = makeHeader(memberItemKey(first.id, member), true, false,
                                    EngineUpdateReason::RekordboxPathShared,
                                    "Put it into " + engineName + "'s " + quoted(path), "member:in:after:" + anchor,
                                    std::move(deps));
            choice.emplace_back(std::move(add));
            holds.insert(member);
            anchor = member;
        }

        EngineUpdateConflict conflict;
        conflict.header = makeHeader(key, false, true, EngineUpdateReason::RekordboxPathShared,
                                     "rekordbox has " + count + " playlists spelled " + quoted(leafName(path))
                                         + " here; " + engineName + " can hold one at this path",
                                     state, parentDeps);
        // Their tracks in rekordbox order, the first list's first: the
        // row's details say which go where.
        if (engineHas) {
            conflict.rekordboxSide = "Add the tracks of all " + count + " of them to " + engineName + "'s "
                + quoted(path);
            conflict.engineSide = "Leave " + engineName + "'s " + quoted(path) + " as it is";
        } else {
            conflict.rekordboxSide = "Create one " + engineName + " playlist " + quoted(path) + " holding all " + count
                + " of them";
            conflict.engineSide = "Leave " + engineName + " without " + quoted(path);
        }
        conflict.rekordboxChoice = std::move(choice);
        out.conflicts.push_back(std::move(conflict));
        settle({key});
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
        // Paths more than one rekordbox playlist spells, each group in id
        // order, and the member the baseline pairs with Engine's playlist
        // there (sharedPath).
        std::map<std::string, std::vector<const BaselinePlaylist *>> shared;
        for (const auto &playlist : now.playlists) {
            shared[playlist.path].push_back(&playlist);
        }
        std::erase_if(shared, [](const auto &entry) { return entry.second.size() < 2; });
        // Folders hold no members, so a group of folders is one folder:
        // the paired one, or the lowest id, stands for the others.
        std::map<std::string, const BaselinePlaylist *> pairedAt;
        for (auto &[path, group] : shared) {
            std::sort(group.begin(), group.end(),
                      [](const BaselinePlaylist *a, const BaselinePlaylist *b) { return a->id < b->id; });
            if (base && countAt(path) <= 1) {
                for (const BaselinePlaylist *p : group) {
                    const BaselinePlaylist *b = base->findPlaylist(p->id);
                    if (b && b->path == path) {
                        pairedAt[path] = p;
                        break;
                    }
                }
            }
            const bool folders = std::all_of(group.begin(), group.end(), [](const BaselinePlaylist *p) {
                return p->folder;
            });
            if (folders && !pairedAt.count(path)) {
                pairedAt[path] = group.front();
            }
        }
        // The paired one first among its group, so the others can follow it.
        std::map<std::string, std::vector<std::size_t>> slots;
        for (std::size_t slot = 0; slot < order.size(); ++slot) {
            const auto &path = now.playlists[order[slot]].path;
            if (pairedAt.count(path)) {
                slots[path].push_back(slot);
            }
        }
        for (const auto &[path, places] : slots) {
            const BaselinePlaylist *paired = pairedAt.at(path);
            for (const std::size_t slot : places) {
                if (&now.playlists[order[slot]] == paired) {
                    std::swap(order[slot], order[places.front()]);
                    break;
                }
            }
        }
        std::set<std::string> sharedDone;

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

            if (const auto group = shared.find(r.path); group != shared.end()) {
                const auto pairedIt = pairedAt.find(r.path);
                const BaselinePlaylist *paired = pairedIt == pairedAt.end() ? nullptr : pairedIt->second;
                if (paired != &r) {
                    if (paired && paired->folder && r.folder) {
                        status[r.id] = status[paired->id];
                    } else if (sharedDone.insert(r.path).second) {
                        sharedPath(group->second, paired, parentDeps);
                    }
                    continue;
                }
            }
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
                conflict.engineSide = "Leave " + engineName + " without " + quoted(r.path);
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
                    conflict.rekordboxSide = "Rename " + quoted(*guess) + " to " + quoted(r.path) + " in " + engineName;
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
                    conflict.engineSide = "Keep " + quoted(b->path) + " in " + engineName;
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
                    conflict.rekordboxSide = "Take it out of " + quoted(path) + " in " + engineName;
                    conflict.engineSide = "Keep it in " + quoted(path);
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
            // The record holds the playlist but not the members out of
            // place: no record says where those belong.
            bool unrecordedMembers = false;
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
                        keptRow.header = makeHeader(orderItemKey(r.id), false, false, EngineUpdateReason::EngineOwn,
                                                    engineName + " changed the order of " + quoted(path)
                                                        + " after Seabass last recorded the stick",
                                                    playlistState(&r));
                        out.engineOwnKept.push_back(std::move(keptRow));
                        moved.clear();
                    } else if (rb == bb && eb == bb) {
                        // The recorded members are in the recorded order on
                        // both sides; only members the record lacks differ
                        // in place. Neither side's change, as far as
                        // anything recorded says.
                        orderMode = OrderMode::Conflict;
                        orderReason = EngineUpdateReason::NoBaselineOrder;
                        unrecordedMembers = true;
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
            // before it that Engine's copy will hold. An order in question
            // is one conflict for the playlist, its rekordbox choice every
            // move: the removes, then the adds in rekordbox's order.
            std::string anchor;
            std::vector<MembershipEdit> orderRemoves;
            std::vector<MembershipEdit> orderAdds;
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
                            // Key and words are the conflict's, below.
                            finish(remove, true, orderReason, {}, key);
                            finish(add, true, orderReason, {}, key);
                            orderRemoves.push_back(std::move(remove));
                            orderAdds.push_back(std::move(add));
                        }
                    }
                    anchor = key;
                } else if (eSet.count(key)) {
                    anchor = key;
                }
            }
            if (!orderAdds.empty()) {
                const std::size_t n = orderAdds.size();
                const std::string tracks = n == 1 ? std::string("1 track") : std::to_string(n) + " tracks";
                std::size_t unrecorded = 0;
                for (const auto &key : common) {
                    unrecorded += bSet.count(key) ? 0 : 1;
                }
                std::string why;
                if (orderReason == EngineUpdateReason::BothChanged) {
                    why = "Both sides changed the order of " + quoted(path) + " since Seabass last recorded the stick; "
                        + tracks + (n == 1 ? " sits" : " sit") + " elsewhere in " + engineName + "'s than in "
                        "rekordbox's";
                } else if (unrecordedMembers) {
                    why = "Seabass's record of " + quoted(path) + " does not list " + std::to_string(unrecorded)
                        + " of the tracks both sides hold, and the two orders differ only around "
                        + (unrecorded == 1 ? "that one" : "those");
                } else {
                    why = "No earlier record of this stick: " + tracks + (n == 1 ? " sits" : " sit") + " elsewhere in "
                        + engineName + "'s " + quoted(path) + " than in rekordbox's";
                }
                const std::string side = "Put " + quoted(path) + " in rekordbox's order (" + tracks
                    + (n == 1 ? " moves)" : " move)");
                // The playlist's order, as rekordbox has it: what the
                // item's state is.
                std::string order = "order:";
                for (const auto &key : rm) {
                    order += key + '\n';
                }
                EngineUpdateConflict conflict;
                conflict.header = makeHeader(orderItemKey(r.id), false, true, orderReason, why, order, st.deps);
                conflict.rekordboxSide = side;
                conflict.engineSide = "Keep " + engineName + "'s order";
                for (auto *list : {&orderRemoves, &orderAdds}) {
                    for (auto &edit : *list) {
                        edit.header.key = conflict.header.key;
                        edit.header.reasonText = side;
                        conflict.rekordboxChoice.emplace_back(std::move(edit));
                    }
                }
                out.conflicts.push_back(std::move(conflict));
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
                    c.rekordboxSide = rv ? "Set the rating in " + engineName + " to " + starsText(rv) + " (rekordbox's)"
                                         : "Clear the rating in " + engineName + " (rekordbox has none)";
                    c.engineSide = ev ? "Keep " + engineName + "'s " + starsText(ev)
                                      : "Leave " + engineName + " without a rating";
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
                                                                    c.rekordboxSide));
                            c.engineSide = "Keep " + engineName + "'s " + starsText(ev)
                                + " and write the rating back onto rekordbox";
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
                            toEngine(rv, true, EngineUpdateReason::BothChanged, c.rekordboxSide));
                        out.conflicts.push_back(std::move(c));
                    }
                } else if (rv && ev) {
                    // No record: only two ratings that are both set and
                    // differ are a question.
                    auto c = conflictOf(EngineUpdateReason::NoBaselineValuesDiffer,
                                        "Ratings differ (rekordbox " + starsText(rv) + ", " + engineName + " "
                                            + starsText(ev) + ") and no earlier record says which side changed");
                    c.rekordboxChoice.emplace_back(
                        toEngine(rv, true, EngineUpdateReason::NoBaselineValuesDiffer, c.rekordboxSide));
                    out.conflicts.push_back(std::move(c));
                } else if (ev) {
                    // Clearing Engine's rating would be destructive and
                    // unattributed: it is Engine's own until a record says
                    // otherwise.
                    EngineOwnItem kept;
                    kept.engine = *e;
                    kept.header = makeHeader(itemKeyText, false, false, EngineUpdateReason::NoBaselineEngineValue,
                                             engineName + "'s own rating (" + starsText(ev)
                                                 + "): rekordbox has none, and with no earlier record nothing is "
                                                   "removed",
                                             state);
                    out.engineOwnKept.push_back(std::move(kept));
                } else {
                    out.metadataToEngine.push_back(toEngine(rv, true, EngineUpdateReason::NoBaselineAddition,
                                                            "No earlier record of this stick: rekordbox has a rating ("
                                                                + starsText(rv) + ") and " + engineName
                                                                + " has none"));
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
                    c.rekordboxSide = r->comment.empty()
                        ? "Clear the comment in " + engineName + " (rekordbox has none)"
                        : "Set the comment in " + engineName + " to " + shortQuoted(r->comment) + " (rekordbox's)";
                    c.engineSide = e->comment.empty() ? "Leave " + engineName + " without a comment"
                                                      : "Keep " + engineName + "'s comment " + shortQuoted(e->comment);
                    c.rekordboxChoice.emplace_back(toEngine(true, reason, c.rekordboxSide));
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
                } else if (!r->comment.empty() && !e->comment.empty()) {
                    out.conflicts.push_back(conflictOf(EngineUpdateReason::NoBaselineValuesDiffer,
                                                       "Comments differ and no earlier record says which side "
                                                       "changed"));
                } else if (!e->comment.empty()) {
                    EngineOwnItem kept;
                    kept.engine = *e;
                    kept.header = makeHeader(itemKeyText, false, false, EngineUpdateReason::NoBaselineEngineValue,
                                             engineName
                                                 + "'s own comment: rekordbox has none, and with no earlier record "
                                                   "nothing is removed",
                                             state);
                    out.engineOwnKept.push_back(std::move(kept));
                } else {
                    out.metadataToEngine.push_back(toEngine(true, EngineUpdateReason::NoBaselineAddition,
                                                            "No earlier record of this stick: rekordbox has a comment "
                                                            "and "
                                                                + engineName + " has none"));
                }
            }
        }
    }

    bool declinedAt(const CueItemState &item) const
    {
        if (!base) {
            return false;
        }
        const auto it = base->declined.find(item.key);
        return it != base->declined.end() && it->second == item.rekordboxState;
    }

    // A cue row's header: the first item names it, every item is listed.
    static EngineUpdateItemHeader cueHeader(const std::vector<CueItemState> &items, bool checked, bool conflict,
                                            EngineUpdateReason reason, std::string text,
                                            std::vector<std::string> dependsOn = {})
    {
        EngineUpdateItemHeader header =
            makeHeader(items.front().key, checked, conflict, reason, std::move(text), "", std::move(dependsOn));
        header.rekordboxState = items.front().rekordboxState;
        header.cueItems = items;
        return header;
    }

    static SyncPlan cuePlan(const Track &r, const Track &e, SyncPlan::Direction direction, std::vector<CuePoint> cues,
                            double toleranceMs)
    {
        SyncPlan plan;
        plan.match = SyncMatch{r, e};
        const bool targetEmpty = direction == SyncPlan::Direction::ToB ? e.cues.empty() : r.cues.empty();
        const SyncPlan::Kind onlySource =
            direction == SyncPlan::Direction::ToB ? SyncPlan::Kind::AOnly : SyncPlan::Kind::BOnly;
        plan.kind = targetEmpty ? onlySource : SyncPlan::Kind::Conflict;
        plan.direction = direction;
        plan.cuesToApply = std::move(cues);
        plan.positionToleranceMs = toleranceMs;
        return plan;
    }

    // Cues of every pair whose two rows exist. A new track's cues ride in
    // its TrackToAdd; a file Engine lists twice is its own conflict.
    void planCues()
    {
        for (const auto &[key, r] : rByKey) {
            const Track *e = engineRow(key);
            if (!e || duplicateKeys.count(key) || addedKeys.count(key)) {
                continue;
            }
            const BaselineTrack *b = base ? base->findTrack(key) : nullptr;
            if (b) {
                planTrackCues(key, *r, *e, *b);
            } else {
                planTrackCuesWithoutBaseline(key, *r, *e);
            }
        }
    }

    // Every cue item of a pair, for a row of the fallback: rekordbox's
    // pads and memory cues, and the pads and cue point only Engine has.
    std::vector<CueItemState> fallbackItems(const std::string &key, const Track &r, const Track &e,
                                            double toleranceMs) const
    {
        const std::vector<CuePoint> rC = withoutJunkCues(r.cues);
        std::vector<CuePoint> eHot;
        std::vector<CuePoint> eMain;
        for (const CuePoint &cue : withoutJunkCues(e.cues)) {
            (isHotCue(cue) ? eHot : eMain).push_back(cue);
        }
        std::map<int, std::vector<CuePoint>> pads;
        std::vector<CuePoint> memory;
        for (const CuePoint &cue : rC) {
            if (isHotCue(cue)) {
                pads[cue.hotCueNumber].push_back(cue);
            } else {
                memory.push_back(cue);
            }
        }
        for (const CuePoint &cue : cuesFromEngine(eHot, rC, toleranceMs).hotCues) {
            pads[cue.hotCueNumber];
        }
        std::vector<CueItemState> items;
        std::set<std::string> seen;
        const auto add = [&](const std::string &itemKeyText, const std::string &state) {
            if (seen.insert(itemKeyText).second) {
                items.push_back(CueItemState{itemKeyText, engineUpdateStateHash(state)});
            }
        };
        for (const auto &[pad, cues] : pads) {
            add(cueItemKey(key, padCue(pad)), padState(pad, cues));
        }
        for (const CuePoint &cue : byPosition(memory)) {
            add(cueItemKey(key, cue), memoryState(&cue));
        }
        for (const CuePoint &cue : eMain) {
            const bool rekordboxHas = std::any_of(memory.begin(), memory.end(), [&](const CuePoint &own) {
                return sameCuePlace(own, cue, toleranceMs);
            });
            if (!rekordboxHas) {
                add(cueItemKey(key, cue), memoryState(nullptr));
            }
        }
        return items;
    }

    // No record of the track: exactly what Sync Cue Points decides for the
    // pair (SyncPlanner::plan), so the first run is that page's behaviour.
    void planTrackCuesWithoutBaseline(const std::string &key, const Track &r, const Track &e)
    {
        const SyncPlan plan = SyncPlanner::plan(SyncMatch{r, e}, {}, {});
        if (!plan.needsChoice && plan.direction == SyncPlan::Direction::None) {
            return;
        }
        const std::vector<CueItemState> items = fallbackItems(key, r, e, plan.positionToleranceMs);
        if (items.empty()) {
            return;  // nothing to name it by; cannot happen with cues on either side
        }
        const std::string engineName = catalogDisplayName("engine");
        if (plan.needsChoice) {
            EngineUpdateConflict conflict;
            conflict.header = cueHeader(items, false, true, EngineUpdateReason::NoBaselineCues, plan.reasonText);
            conflict.pathKey = key;
            conflict.rekordboxSide = "Use rekordbox's cues in " + engineName;
            conflict.engineSide = "Copy " + engineName + "'s cues to rekordbox";
            SyncPlan toEngine = plan;
            toEngine.needsChoice = false;
            toEngine.direction = SyncPlan::Direction::ToB;
            toEngine.cuesToApply = plan.cuesIfAWins;
            conflict.rekordboxChoice.emplace_back(
                CueEdit{cueHeader(items, true, false, EngineUpdateReason::NoBaselineCues, conflict.rekordboxSide),
                        toEngine, key});
            SyncPlan toRekordbox = plan;
            toRekordbox.needsChoice = false;
            toRekordbox.direction = SyncPlan::Direction::ToA;
            toRekordbox.cuesToApply = plan.cuesIfBWins;
            conflict.engineChoice.emplace_back(CueEdit{
                cueHeader(items, true, false, EngineUpdateReason::NoBaselineCues, conflict.engineSide),
                toRekordbox, key});
            out.conflicts.push_back(std::move(conflict));
            return;
        }
        const bool ontoEngine = plan.direction == SyncPlan::Direction::ToB;
        // Engine's cues over rekordbox's 0:00 ones: said as the baseline's
        // rule says it, the record or its absence changes nothing.
        if (plan.reason == SyncPlan::Reason::StartCueOverEngine) {
            out.cuesToRekordbox.push_back(
                CueEdit{cueHeader(items, true, false, EngineUpdateReason::StartCueOverEngine, plan.reasonText), plan,
                        key});
            return;
        }
        CueEdit edit{cueHeader(items, true, false, EngineUpdateReason::NoBaselineCues,
                               ontoEngine ? "No earlier record of this stick: rekordbox's cues go to " + engineName
                                       + ", as Sync Cue Points decides"
                                          : "No earlier record of this stick: " + engineName
                                       + "'s cues go to rekordbox, as Sync Cue Points decides"),
                     plan, key};
        (ontoEngine ? out.cuesToEngine : out.cuesToRekordbox).push_back(std::move(edit));
    }

    // The three-way merge of one track's cues. Each pad is an item, each
    // memory cue or loop another, named by the baseline's place. Engine is
    // read in rekordbox's terms (cuesFromEngine against rekordbox's cues
    // and the baseline's memory cues): a pad at a memory cue's place is
    // that memory cue. What rekordbox changed is applied onto Engine's
    // set, which is translated back (translateCuesForEngine against
    // Engine's cues, so nothing shuffles pads) and written only when it
    // differs from what Engine has.
    void planTrackCues(const std::string &key, const Track &r, const Track &e, const BaselineTrack &b)
    {
        const std::string engineName = catalogDisplayName("engine");
        const CueTolerance tolerance = cueToleranceFor(r.bpm, e.bpm);
        const double tol = tolerance.ms;
        const std::vector<CuePoint> rC = withoutJunkCues(r.cues);
        const std::vector<CuePoint> eC = withoutJunkCues(e.cues);

        std::map<int, std::vector<CuePoint>> rHot;
        std::map<int, std::vector<const BaselineCue *>> bHot;
        std::vector<CuePoint> rMem;
        std::vector<const BaselineCue *> bMem;
        for (const CuePoint &cue : rC) {
            if (isHotCue(cue)) {
                rHot[cue.hotCueNumber].push_back(cue);
            } else {
                rMem.push_back(cue);
            }
        }
        for (const BaselineCue &cue : b.cues) {
            if (isJunkCue(cue.cue)) {
                continue;
            }
            if (isHotCue(cue.cue)) {
                bHot[cue.cue.hotCueNumber].push_back(&cue);
            } else {
                bMem.push_back(&cue);
            }
        }
        rMem = byPosition(rMem);
        std::stable_sort(bMem.begin(), bMem.end(), [](const BaselineCue *x, const BaselineCue *y) {
            return x->cue.positionMs < y->cue.positionMs;
        });

        // Engine in rekordbox's terms. The cue point (Engine's one memory
        // cue) is kept apart: it holds a memory cue rekordbox has, but no
        // write removes it and none is planned for it.
        std::vector<CuePoint> reference = rC;
        for (const BaselineCue *cue : bMem) {
            const bool inR = std::any_of(rMem.begin(), rMem.end(),
                                         [&](const CuePoint &own) { return sameCuePlace(own, cue->cue, tol); });
            if (!inR) {
                reference.push_back(cue->cue);
            }
        }
        std::vector<CuePoint> eHotCues;
        std::vector<CuePoint> eMain;
        for (const CuePoint &cue : eC) {
            (isHotCue(cue) ? eHotCues : eMain).push_back(cue);
        }
        const CuesFromEngine seen = cuesFromEngine(eHotCues, reference, tol);
        std::map<int, std::vector<CuePoint>> eHot;
        for (const CuePoint &cue : seen.hotCues) {
            eHot[cue.hotCueNumber].push_back(cue);
        }
        const std::vector<CuePoint> leftOut = translateCuesForEngine(rC, eC, tol).leftOut;
        // rekordbox's cues at 0:00 on pads where Engine holds a real cue:
        // export noise over Engine's, never a change of rekordbox's
        // (startCuesOverEngine). Engine's cue goes back onto rekordbox.
        const std::vector<StartCueOverEngine> startCues = startCuesOverEngine(r.cues, seen.hotCues);
        const auto startCueOn = [&](int pad) -> const StartCueOverEngine * {
            for (const StartCueOverEngine &over : startCues) {
                if (over.pad == pad) {
                    return &over;
                }
            }
            return nullptr;
        };

        // The target in rekordbox's terms, Engine's set to begin with.
        std::map<int, std::vector<CuePoint>> tHot = eHot;
        std::vector<CuePoint> tMem = seen.memoryCues;

        const auto samePad = [&](const std::vector<CuePoint> &x, const std::vector<CuePoint> &y) {
            return sameCuesForSync(x, y, tol);
        };
        const auto within = [&](const std::vector<CuePoint> &list, const CuePoint &cue) {
            return std::count_if(list.begin(), list.end(),
                                 [&](const CuePoint &other) { return sameCuePlace(other, cue, tol); });
        };
        const auto cuesOf = [](const std::vector<const BaselineCue *> &list) {
            std::vector<CuePoint> cues;
            for (const BaselineCue *cue : list) {
                cues.push_back(cue->cue);
            }
            return cues;
        };
        const auto originOf = [](const std::vector<const BaselineCue *> &list) {
            // One origin for the pad: all its cues agree, else nobody knows.
            const ValueOrigin origin = list.empty() ? ValueOrigin::Unknown : list.front()->origin;
            for (const BaselineCue *cue : list) {
                if (cue->origin != origin) {
                    return ValueOrigin::Unknown;
                }
            }
            return origin;
        };
        const auto removeMemory = [tol](std::vector<CuePoint> &list, const CuePoint &cue) {
            std::erase_if(list, [&](const CuePoint &other) { return sameCuePlace(other, cue, tol); });
        };
        const auto keptAsEngineOwn = [&](const std::string &itemKeyText, const std::string &state, std::string text) {
            EngineOwnItem kept;
            kept.engine = e;
            kept.header = makeHeader(itemKeyText, false, false, EngineUpdateReason::EngineOwn,
                                     std::move(text) + " after Seabass last recorded the stick", state);
            out.engineOwnKept.push_back(std::move(kept));
        };

        struct Pending
        {
            CueItemState item;
            EngineUpdateReason reason;
            std::string text;
            // A conflict's two buttons for this one cue: the object, the
            // action and the value ("Set pad 3 in Engine to 1:07.751,
            // rekordbox's position" / "Keep pad 3 in Engine at 0:30.251").
            std::string rekordboxLabel;
            std::string engineLabel;
        };
        using Target = std::pair<std::map<int, std::vector<CuePoint>> *, std::vector<CuePoint> *>;
        std::vector<Pending> toEngine;
        std::vector<Pending> restored;
        std::vector<Pending> conflicting;
        std::vector<std::function<void(Target)>> rekordboxWay;  // each conflict item resolved rekordbox's way
        std::vector<CuePoint> restoreCues;  // checked restores
        std::vector<Pending> overStart;             // pads whose 0:00 cue Engine's replaces
        std::vector<StartCueOverEngine> goingBack;  // their cues, for every write onto rekordbox
        std::vector<CuePoint> unknownCues;  // Engine's way of the conflicts nobody recorded the origin of

        // Pads.
        std::set<int> pads;
        for (const auto *map : {&rHot, &eHot}) {
            for (const auto &[pad, _] : *map) {
                pads.insert(pad);
            }
        }
        for (const auto &[pad, _] : bHot) {
            pads.insert(pad);
        }
        for (const int pad : pads) {
            if (pad < 1 || pad > EngineHotCuePads) {
                continue;  // Engine has eight pads; nothing beyond them is compared or written
            }
            const std::vector<CuePoint> rn = rHot.count(pad) ? rHot.at(pad) : std::vector<CuePoint>{};
            const std::vector<CuePoint> en = eHot.count(pad) ? eHot.at(pad) : std::vector<CuePoint>{};
            const std::vector<const BaselineCue *> bnRows =
                bHot.count(pad) ? bHot.at(pad) : std::vector<const BaselineCue *>{};
            const std::vector<CuePoint> bn = cuesOf(bnRows);
            if (samePad(rn, en)) {
                continue;
            }
            const std::string itemKeyText = cueItemKey(key, padCue(pad));
            const std::string state = padState(pad, rn);
            const CueItemState item{itemKeyText, engineUpdateStateHash(state)};
            const std::string padName = "pad " + std::to_string(pad);
            // Before the three-way rule: whatever the baseline says, a 0:00
            // cue over Engine's is not rekordbox removing or moving it, nor
            // a conflict, nor Engine's own to leave alone.
            if (const StartCueOverEngine *over = startCueOn(pad)) {
                if (declinedAt(item)) {
                    cueSuppressed.insert(item.key);
                    continue;
                }
                overStart.push_back(Pending{item, EngineUpdateReason::StartCueOverEngine, {}});
                goingBack.push_back(*over);
                continue;
            }
            if (samePad(bn, rn)) {
                keptAsEngineOwn(itemKeyText, state,
                                bn.empty()   ? engineName + " set " + padName + " (" + places(en) + ")"
                                : en.empty() ? engineName + " cleared " + padName + " (" + places(bn) + ")"
                                             : engineName + " set " + padName + " to " + places(en) + " (was "
                                        + places(bn) + ")");
                continue;
            }
            if (declinedAt(item)) {
                cueSuppressed.insert(item.key);
                continue;
            }
            if (samePad(bn, en)) {
                if (!rn.empty()) {
                    tHot[pad] = rn;
                    toEngine.push_back(bn.empty() ? Pending{item, EngineUpdateReason::RekordboxAdded,
                                                            "rekordbox set " + padName + " (" + places(rn) + ")"}
                                                  : Pending{item, EngineUpdateReason::RekordboxChanged,
                                                            "rekordbox set " + padName + " to " + places(rn)
                                                                + " (was " + places(bn) + ")"});
                    continue;
                }
                switch (originOf(bnRows)) {
                case ValueOrigin::Seabass:
                    restoreCues.insert(restoreCues.end(), bn.begin(), bn.end());
                    restored.push_back(Pending{item, EngineUpdateReason::ExportDropped, {}});
                    break;
                case ValueOrigin::Rekordbox:
                    tHot.erase(pad);
                    toEngine.push_back(Pending{item, EngineUpdateReason::RekordboxRemoved,
                                               "rekordbox cleared " + padName + " (" + places(bn) + ")"});
                    break;
                case ValueOrigin::Unknown:
                    conflicting.push_back(Pending{item, EngineUpdateReason::OriginUnknown,
                                                  "rekordbox no longer has " + padName + " (" + places(bn)
                                                      + "), which " + engineName
                                                      + " has, and nothing recorded whether Seabass or rekordbox "
                                                        "put it there",
                                                  "Remove " + padName + " from " + engineName
                                                      + " (rekordbox has no cue there)",
                                                  "Keep " + padName + " in " + engineName
                                                      + " and write it back onto rekordbox"});
                    rekordboxWay.push_back([pad](Target t) { t.first->erase(pad); });
                    unknownCues.insert(unknownCues.end(), bn.begin(), bn.end());
                    break;
                }
                continue;
            }
            // All three differ.
            conflicting.push_back(Pending{item, EngineUpdateReason::BothChanged,
                                          describePadDifference(rn, en, "rekordbox", engineName, tolerance)
                                              + "; both sides changed it since Seabass last recorded the stick "
                                                "(recorded "
                                              + places(bn) + ")",
                                          rn.empty() ? "Clear " + padName + " in " + engineName
                                                  + " (rekordbox has no cue there)"
                                                     : "Set " + padName + " in " + engineName + " to " + places(rn)
                                                  + ", rekordbox's position",
                                          en.empty() ? "Leave " + padName + " in " + engineName + " empty"
                                                     : "Keep " + padName + " in " + engineName + " at " + places(en)});
            rekordboxWay.push_back([pad, rn](Target t) {
                if (rn.empty()) {
                    t.first->erase(pad);
                } else {
                    (*t.first)[pad] = rn;
                }
            });
        }

        // Memory cues and loops: present or not, by place.
        std::vector<bool> rUsed(rMem.size(), false);
        for (const BaselineCue *row : bMem) {
            const CuePoint &bc = row->cue;
            std::size_t ri = rMem.size();
            for (std::size_t i = 0; i < rMem.size(); ++i) {
                if (!rUsed[i] && sameCuePlace(rMem[i], bc, tol)) {
                    ri = i;
                    break;
                }
            }
            const std::string itemKeyText = cueItemKey(key, bc);
            if (ri < rMem.size()) {
                rUsed[ri] = true;
                const CuePoint &rc = rMem[ri];
                if (within(seen.memoryCues, rc) > 0 || within(eMain, rc) > 0 || within(leftOut, rc) > 0) {
                    continue;
                }
                keptAsEngineOwn(itemKeyText, memoryState(&rc), engineName + " took out " + memoryName(bc));
                continue;
            }
            // rekordbox lacks it. Engine's cue point alone is not a pad to
            // take out, so only a pad counts as Engine having it.
            const auto translations = within(seen.memoryCues, bc);
            if (translations == 0) {
                continue;
            }
            const CueItemState item{itemKeyText, engineUpdateStateHash(memoryState(nullptr))};
            if (declinedAt(item)) {
                cueSuppressed.insert(item.key);
                continue;
            }
            // Every pad Engine has there sits on a pad Engine DJ's import
            // would not have given the cue: a hot cue of Engine's own as
            // likely as the translation.
            const auto uncertainHere = std::count_if(seen.uncertain.begin(), seen.uncertain.end(),
                                                     [&](const CuesFromEngine::Uncertain &u) {
                                                         return sameCuePlace(u.memory, bc, tol);
                                                     });
            const bool uncertainOnly = uncertainHere >= translations;
            if (row->origin == ValueOrigin::Seabass) {
                restoreCues.push_back(bc);
                restored.push_back(Pending{item, EngineUpdateReason::ExportDropped, {}});
            } else if (uncertainOnly) {
                int pad = 0;
                for (const auto &u : seen.uncertain) {
                    if (sameCuePlace(u.memory, bc, tol)) {
                        pad = u.pad.hotCueNumber;
                        break;
                    }
                }
                conflicting.push_back(Pending{item, EngineUpdateReason::EngineMemoryOrHotCue,
                                              engineName + " pad " + std::to_string(pad) + " sits where rekordbox had "
                                                  + memoryName(bc)
                                                  + ", which it no longer has: that cue on " + engineName
                                                  + ", or a hot cue of " + engineName + "'s own?",
                                              "Remove pad " + std::to_string(pad) + " from " + engineName
                                                  + " (rekordbox no longer has " + memoryName(bc) + ")",
                                              "Keep pad " + std::to_string(pad) + " in " + engineName
                                                  + " as a hot cue of its own"});
                rekordboxWay.push_back([removeMemory, bc](Target t) { removeMemory(*t.second, bc); });
            } else if (row->origin == ValueOrigin::Rekordbox) {
                removeMemory(tMem, bc);
                toEngine.push_back(
                    Pending{item, EngineUpdateReason::RekordboxRemoved, "rekordbox removed " + memoryName(bc)});
            } else {
                conflicting.push_back(Pending{item, EngineUpdateReason::OriginUnknown,
                                              "rekordbox no longer has " + memoryName(bc) + ", which " + engineName
                                                  + " has, and nothing recorded whether Seabass or rekordbox put "
                                                    "it there",
                                              "Remove " + memoryName(bc) + " from " + engineName
                                                  + " (rekordbox has no cue there)",
                                              "Keep " + memoryName(bc) + " in " + engineName
                                                  + " and write it back onto rekordbox"});
                rekordboxWay.push_back([removeMemory, bc](Target t) { removeMemory(*t.second, bc); });
                unknownCues.push_back(bc);
            }
        }
        for (std::size_t i = 0; i < rMem.size(); ++i) {
            const CuePoint &rc = rMem[i];
            if (rUsed[i] || within(seen.memoryCues, rc) > 0 || within(eMain, rc) > 0 || within(leftOut, rc) > 0) {
                continue;  // level, or no pad is free for it on Engine
            }
            const CueItemState item{cueItemKey(key, rc), engineUpdateStateHash(memoryState(&rc))};
            if (declinedAt(item)) {
                cueSuppressed.insert(item.key);
                continue;
            }
            tMem.push_back(rc);
            toEngine.push_back(Pending{item, EngineUpdateReason::RekordboxAdded, "rekordbox added " + memoryName(rc)});
        }

        // Rows.
        const auto itemsOf = [](const std::vector<Pending> &list) {
            std::vector<CueItemState> items;
            for (const auto &p : list) {
                items.push_back(p.item);
            }
            return items;
        };
        // The target written onto Engine, or nothing when Engine already
        // holds it.
        const auto ontoEngine = [&](const std::map<int, std::vector<CuePoint>> &hot,
                                    const std::vector<CuePoint> &memory) -> std::optional<SyncPlan> {
            std::vector<CuePoint> cues;
            for (const auto &[pad, list] : hot) {
                cues.insert(cues.end(), list.begin(), list.end());
            }
            cues.insert(cues.end(), memory.begin(), memory.end());
            const EngineCueTranslation translation = translateCuesForEngine(cues, eC, tol);
            if (sameCuesForSync(translation.cues, eC, tol)) {
                return std::nullopt;
            }
            SyncPlan plan =
                cuePlan(r, e, SyncPlan::Direction::ToB, keepExistingColours(translation.cues, eC, tol), tol);
            plan.cuesLeftOut = translation.leftOut;
            return plan;
        };
        // rekordbox's cues with Engine's over the 0:00 ones and `extra`
        // added: every write onto rekordbox carries Engine's cues back over
        // the start cues, a conflict's choice too, as it is the last write.
        const auto ontoRekordbox = [&](const std::vector<CuePoint> &extra) {
            std::vector<CuePoint> cues = withEngineCuesOverStartCues(r.cues, goingBack);
            cues.insert(cues.end(), extra.begin(), extra.end());
            return cuePlan(r, e, SyncPlan::Direction::ToA, std::move(cues), tol);
        };

        std::vector<std::string> written;
        if (!toEngine.empty()) {
            if (auto plan = ontoEngine(tHot, tMem)) {
                const EngineUpdateReason firstReason = toEngine.front().reason;
                const bool oneReason = std::all_of(toEngine.begin(), toEngine.end(),
                                                   [&](const Pending &p) { return p.reason == firstReason; });
                std::string text;
                const std::size_t shown = std::min<std::size_t>(toEngine.size(), 3);
                for (std::size_t i = 0; i < shown; ++i) {
                    text += (i == 0 ? "" : "; ") + toEngine[i].text;
                }
                if (toEngine.size() > shown) {
                    text += "; and " + std::to_string(toEngine.size() - shown) + " more";
                }
                CueEdit edit{cueHeader(itemsOf(toEngine), true, false,
                                       oneReason ? toEngine.front().reason : EngineUpdateReason::RekordboxChanged,
                                       text + since()),
                             std::move(*plan), key};
                written.push_back(edit.header.key);
                out.cuesToEngine.push_back(std::move(edit));
            }
        }
        // One row onto rekordbox: the restores and Engine's cues over the
        // 0:00 ones are one write of the track's whole cue set.
        if (!restored.empty() || !overStart.empty()) {
            std::string text;
            if (!restored.empty()) {
                const std::size_t n = restoreCues.size();
                text = "rekordbox's export dropped " + std::to_string(n)
                    + (n == 1 ? " cue Seabass had synced from " + engineName + "; it goes back"
                              : " cues Seabass had synced from " + engineName + "; they go back");
            }
            if (!overStart.empty()) {
                text += (text.empty() ? "" : "; ") + describeStartCuesOverEngine(goingBack);
            }
            std::vector<Pending> items = restored;
            items.insert(items.end(), overStart.begin(), overStart.end());
            CueEdit edit{cueHeader(itemsOf(items), true, false,
                                   restored.empty() ? EngineUpdateReason::StartCueOverEngine
                                                    : EngineUpdateReason::ExportDropped,
                                   std::move(text)),
                         ontoRekordbox(restoreCues), key};
            if (restored.empty()) {
                // The plan says it as SyncPlanner's would.
                edit.plan.reason = SyncPlan::Reason::StartCueOverEngine;
                edit.plan.reasonText = edit.header.reasonText;
            }
            written.push_back(edit.header.key);
            out.cuesToRekordbox.push_back(std::move(edit));
        }
        if (!conflicting.empty()) {
            std::string text = conflicting.front().text;
            if (conflicting.size() > 1) {
                const std::size_t more = conflicting.size() - 1;
                text += "; and " + std::to_string(more) + (more == 1 ? " more cue differs" : " more cues differ");
            }
            EngineUpdateConflict conflict;
            conflict.header =
                cueHeader(itemsOf(conflicting), false, true, conflicting.front().reason, std::move(text), written);
            conflict.pathKey = key;
            // One cue: its own words. Several on one track: what each
            // side does to all of them (the reason names the first, the
            // details every one).
            if (conflicting.size() == 1) {
                conflict.rekordboxSide = conflicting.front().rekordboxLabel;
                conflict.engineSide = conflicting.front().engineLabel;
            } else {
                const std::string n = std::to_string(conflicting.size());
                conflict.rekordboxSide = "Set these " + n + " cues in " + engineName + " as rekordbox has them";
                conflict.engineSide = unknownCues.empty()
                    ? "Keep these " + n + " cues in " + engineName + " as they are"
                    : "Keep these " + n + " cues in " + engineName + " and write the ones rekordbox lacks back "
                      "onto rekordbox";
            }
            auto hot = tHot;
            auto memory = tMem;
            for (const auto &resolve : rekordboxWay) {
                resolve(Target{&hot, &memory});
            }
            if (auto plan = ontoEngine(hot, memory)) {
                if (conflicting.front().reason == EngineUpdateReason::EngineMemoryOrHotCue) {
                    plan->reason = SyncPlan::Reason::EngineMemoryOrHotCue;
                }
                conflict.rekordboxChoice.emplace_back(CueEdit{
                    cueHeader(itemsOf(conflicting), true, false, conflicting.front().reason, conflict.rekordboxSide),
                    std::move(*plan), key});
            }
            if (!unknownCues.empty()) {
                std::vector<CuePoint> back = restoreCues;
                back.insert(back.end(), unknownCues.begin(), unknownCues.end());
                conflict.engineChoice.emplace_back(CueEdit{
                    cueHeader(itemsOf(conflicting), true, false, conflicting.front().reason, conflict.engineSide),
                    ontoRekordbox(back), key});
            }
            out.conflicts.push_back(std::move(conflict));
        }
    }

    // Declined items stay out while rekordbox's state of them is the one
    // recorded; a row depending on one that stays out stays out too.
    //
    // A cue row stays out when every cue item it covers is declined at its
    // state; its declined items that rekordbox left alone were already
    // kept out of the rows they would have joined (cueSuppressed).
    void suppressDeclined()
    {
        if (!base || base->declined.empty()) {
            return;
        }
        const auto declined = [&](const std::string &key, const std::string &state) {
            const auto it = base->declined.find(key);
            return it != base->declined.end() && it->second == state;
        };
        std::set<std::string> suppressed;
        eachDecidable(out, [&](auto &rows) {
            for (const auto &row : rows) {
                const auto &items = row.header.cueItems;
                if (items.empty() ? declined(row.header.key, row.header.rekordboxState)
                                  : std::all_of(items.begin(), items.end(), [&](const CueItemState &item) {
                                        return declined(item.key, item.rekordboxState);
                                    })) {
                    suppressed.insert(row.header.key);
                }
            }
        });
        const auto record = [&](std::set<std::string> keys) {
            keys.insert(cueSuppressed.begin(), cueSuppressed.end());
            eachDecidable(out, [&](auto &rows) {
                for (const auto &row : rows) {
                    if (suppressed.count(row.header.key)) {
                        for (const auto &item : row.header.cueItems) {
                            keys.insert(item.key);
                        }
                    }
                }
            });
            out.declinedSuppressed.assign(keys.begin(), keys.end());
        };
        if (suppressed.empty()) {
            record(suppressed);
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
        record(suppressed);
        eachDecidable(out, [&](auto &rows) {
            rows.erase(std::remove_if(rows.begin(), rows.end(),
                                      [&](const auto &row) { return suppressed.count(row.header.key) > 0; }),
                       rows.end());
        });
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
