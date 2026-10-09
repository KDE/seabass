// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// domain::EngineUpdatePlanner for tracks, playlists, membership, metadata
// and cues: one case per row of the three-way table and per section of
// docs/sync-after-rekordbox-export-plan.md, the fallback without a
// baseline, and idempotence (plan, apply in memory, plan again is empty).
// Every expected value is pinned by hand from the input as written.

#include "domain/engine_update_planning.hpp"
#include "domain/matching_policy.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

using namespace seabass::domain;

namespace
{

const std::string StickRoot = "/stick/";
const std::string Since = " after Seabass last recorded the stick (export 15132, recorded at 14204)";

std::string stickRelative(const std::string &filePath)
{
    return filePath.rfind(StickRoot, 0) == 0 ? filePath.substr(StickRoot.size()) : std::string();
}

// Lowercase, so a key visibly differs from the path it came from.
std::string lowerKey(const std::string &path)
{
    std::string key = path;
    for (char &c : key) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return key;
}

std::string keyOf(const Track &t)
{
    return lowerKey(stickRelative(t.filePath));
}

Track track(const std::string &format, const std::string &id, const std::string &relative,
            std::vector<PlaylistMembership> playlists = {}, std::optional<int> rating = std::nullopt)
{
    Track t;
    t.format = format;
    t.sourceId = id;
    t.filePath = StickRoot + relative;
    t.filename = relative.substr(relative.rfind('/') + 1);
    t.title = relative;  // distinct per file, so no two are the same song by accident
    t.artist = "Artist";
    t.durationSeconds = 200.0;
    t.fileSizeBytes = 1000;  // present on the stick: no name fallback in matchTracks
    t.playlists = std::move(playlists);
    t.rating = rating;
    return t;
}

Track rb(const std::string &id, const std::string &relative, std::vector<PlaylistMembership> playlists = {},
         std::optional<int> rating = std::nullopt)
{
    return track("rekordbox", id, relative, std::move(playlists), rating);
}

Track en(const std::string &id, const std::string &relative, std::vector<PlaylistMembership> playlists = {},
         std::optional<int> rating = std::nullopt)
{
    return track("engine", id, relative, std::move(playlists), rating);
}

// One stick: both catalogs as the readers give them.
struct World
{
    std::vector<Track> rekordbox;
    std::vector<Track> engine;
    std::vector<PlaylistInfo> rekordboxPlaylists;
    std::vector<EnginePlaylistInfo> enginePlaylists;
    std::map<std::string, std::int64_t> importKey;
    std::set<std::string> missingFiles;  // stick-relative
};

RekordboxBaseline baselineOf(const World &w)
{
    return baselineFrom(w.rekordbox, w.rekordboxPlaylists, 14204, stickRelative, lowerKey);
}

EngineUpdateProposal plan(const World &w, std::optional<RekordboxBaseline> baseline)
{
    EngineUpdateInput in;
    in.rekordbox = w.rekordbox;
    in.engine = w.engine;
    in.rekordboxPlaylists = w.rekordboxPlaylists;
    in.enginePlaylists = w.enginePlaylists;
    in.baseline = std::move(baseline);
    in.currentSequence = 15132;
    in.enginePdbImportKey = w.importKey;
    in.stickRelativeOf = stickRelative;
    in.pathKeyOf = lowerKey;
    const std::set<std::string> missing = w.missingFiles;
    in.fileExists = [missing](const std::string &relative) { return !missing.count(relative); };
    return EngineUpdatePlanner::plan(in);
}

Track *byKey(std::vector<Track> &tracks, const std::string &key)
{
    for (auto &t : tracks) {
        if (keyOf(t) == key) {
            return &t;
        }
    }
    return nullptr;
}

bool underOrAt(const std::string &path, const std::string &root)
{
    return path == root || (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/');
}

std::string movedPath(const std::string &path, const std::string &from, const std::string &to)
{
    return underOrAt(path, from) ? to + path.substr(from.size()) : path;
}

// Applies every checked row, in the staging order of the plan, to the
// World in memory, the way the writers of later steps will to the stick.
// With takeRekordboxChoice, each conflict's rekordbox choice too.
void apply(World &w, const EngineUpdateProposal &p, bool takeRekordboxChoice = false)
{
    // Engine's playlists as ordered key lists.
    std::map<std::string, std::vector<std::string>> lists;
    for (const auto &pl : w.enginePlaylists) {
        if (!pl.folder) {
            lists[pl.path];
        }
    }
    std::map<std::string, std::vector<std::pair<int, std::string>>> entries;
    for (const auto &t : w.engine) {
        for (const auto &m : t.playlists) {
            entries[m.name].emplace_back(m.position, keyOf(t));
        }
    }
    for (auto &[path, list] : entries) {
        std::sort(list.begin(), list.end());
        for (const auto &e : list) {
            lists[path].push_back(e.second);
        }
    }

    std::vector<PlaylistCreate> creates = p.playlistsToCreate;
    std::vector<TrackToAdd> adds = p.tracksToAdd;
    std::vector<PlaylistRename> renames = p.playlistsToRename;
    std::vector<MembershipEdit> members = p.membership;
    std::vector<MetadataEdit> metadata = p.metadataToEngine;
    std::vector<PlaylistDelete> deletes = p.playlistsToDelete;
    std::vector<TrackToRemove> removes = p.tracksToRemove;
    std::vector<MetadataEdit> restores = p.restoresToRekordbox;
    // A track's cue rows first, a conflict's chosen cue row after them:
    // the choice already holds what they write (CueEdit's comment).
    std::vector<CueEdit> cues = p.cuesToEngine;
    cues.insert(cues.end(), p.cuesToRekordbox.begin(), p.cuesToRekordbox.end());
    if (takeRekordboxChoice) {
        for (const auto &c : p.conflicts) {
            for (const auto &edit : c.rekordboxChoice) {
                std::visit(
                    [&](const auto &e) {
                        using T = std::decay_t<decltype(e)>;
                        if constexpr (std::is_same_v<T, PlaylistCreate>) {
                            creates.push_back(e);
                        } else if constexpr (std::is_same_v<T, TrackToAdd>) {
                            adds.push_back(e);
                        } else if constexpr (std::is_same_v<T, PlaylistRename>) {
                            renames.push_back(e);
                        } else if constexpr (std::is_same_v<T, MembershipEdit>) {
                            members.push_back(e);
                        } else if constexpr (std::is_same_v<T, MetadataEdit>) {
                            (e.direction == MetadataEdit::Direction::ToEngine ? metadata : restores).push_back(e);
                        } else if constexpr (std::is_same_v<T, PlaylistDelete>) {
                            deletes.push_back(e);
                        } else if constexpr (std::is_same_v<T, CueEdit>) {
                            cues.push_back(e);
                        } else {
                            static_assert(std::is_same_v<T, TrackToRemove>);
                            removes.push_back(e);
                        }
                    },
                    edit);
            }
        }
    }

    for (const auto &c : creates) {
        w.enginePlaylists.push_back(EnginePlaylistInfo{c.path, c.folder, 1});
        if (!c.folder) {
            lists[c.path];
        }
    }
    for (const auto &a : adds) {
        Track t = a.rekordbox;
        t.format = "engine";
        t.sourceId = "new:" + a.pathKey;
        t.playlists.clear();
        w.importKey[t.sourceId] = 0;
        w.engine.push_back(t);
    }
    for (const auto &r : renames) {
        for (auto &pl : w.enginePlaylists) {
            pl.path = movedPath(pl.path, r.fromPath, r.toPath);
        }
        std::map<std::string, std::vector<std::string>> renamed;
        for (auto &[path, list] : lists) {
            renamed[movedPath(path, r.fromPath, r.toPath)] = list;
        }
        lists = renamed;
    }
    for (const auto &m : members) {
        auto &list = lists.at(m.playlistPath);
        if (m.kind == MembershipEdit::Kind::Remove) {
            const auto it = std::find(list.begin(), list.end(), m.pathKey);
            assert(it != list.end());
            list.erase(it);
            continue;
        }
        if (m.afterPathKey.empty()) {
            list.insert(list.begin(), m.pathKey);
        } else {
            const auto it = std::find(list.begin(), list.end(), m.afterPathKey);
            assert(it != list.end());
            list.insert(it + 1, m.pathKey);
        }
    }
    for (const auto &m : metadata) {
        Track *t = byKey(w.engine, m.pathKey);
        assert(t);
        if (m.field == MetadataEdit::Field::Rating) {
            t->rating = m.rating;
        } else {
            t->comment = m.comment;
        }
    }
    for (const auto &d : deletes) {
        std::erase_if(w.enginePlaylists, [&](const EnginePlaylistInfo &pl) { return underOrAt(pl.path, d.path); });
        std::erase_if(lists, [&](const auto &entry) { return underOrAt(entry.first, d.path); });
    }
    for (const auto &r : removes) {
        std::erase_if(w.engine, [&](const Track &t) { return keyOf(t) == r.pathKey; });
        for (auto &[path, list] : lists) {
            std::erase(list, r.pathKey);
        }
    }
    for (const auto &m : restores) {
        Track *t = byKey(w.rekordbox, m.pathKey);
        assert(t);
        t->rating = m.rating;
    }
    for (const auto &c : cues) {
        const bool ontoEngine = c.plan.direction == SyncPlan::Direction::ToB;
        assert(ontoEngine || c.plan.direction == SyncPlan::Direction::ToA);
        Track *t = byKey(ontoEngine ? w.engine : w.rekordbox, c.pathKey);
        assert(t);
        t->cues = c.plan.cuesToApply;
    }

    for (auto &t : w.engine) {
        t.playlists.clear();
    }
    for (const auto &[path, list] : lists) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            Track *t = byKey(w.engine, list[i]);
            assert(t);
            t->playlists.push_back(PlaylistMembership{path, static_cast<int>(i)});
        }
    }
}

std::vector<std::string> engineOrder(const World &w, const std::string &path)
{
    std::vector<std::pair<int, std::string>> entries;
    for (const auto &t : w.engine) {
        for (const auto &m : t.playlists) {
            if (m.name == path) {
                entries.emplace_back(m.position, keyOf(t));
            }
        }
    }
    std::sort(entries.begin(), entries.end());
    std::vector<std::string> out;
    for (const auto &e : entries) {
        out.push_back(e.second);
    }
    return out;
}

using Deps = std::vector<std::string>;

CuePoint hot(int pad, double ms)
{
    CuePoint c;
    c.kind = CuePoint::Kind::Hot;
    c.hotCueNumber = pad;
    c.positionMs = ms;
    return c;
}

CuePoint mem(double ms)
{
    CuePoint c;
    c.positionMs = ms;
    return c;
}

// A pair at 120 BPM: half a beat is 250 ms.
World cuePair(std::vector<CuePoint> rekordboxCues, std::vector<CuePoint> engineCues,
              const std::string &relative = "Music/A.mp3", const std::string &id = "1")
{
    World w;
    w.rekordbox = {rb(id, relative)};
    w.engine = {en("e" + id, relative)};
    w.rekordbox[0].bpm = 120.0;
    w.engine[0].bpm = 120.0;
    w.rekordbox[0].cues = std::move(rekordboxCues);
    w.engine[0].cues = std::move(engineCues);
    w.importKey = {{"e" + id, 14204}};
    return w;
}

void setOrigin(RekordboxBaseline &base, ValueOrigin origin)
{
    for (auto &t : base.tracks) {
        for (auto &c : t.cues) {
            c.origin = origin;
        }
    }
}

// Kind, pad, place and loop end, in order: colour never counts.
bool sameCueList(const std::vector<CuePoint> &a, const std::vector<CuePoint> &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].kind != b[i].kind || a[i].hotCueNumber != b[i].hotCueNumber || a[i].positionMs != b[i].positionMs
            || a[i].isLoop != b[i].isLoop || a[i].loopEndMs != b[i].loopEndMs) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> itemKeys(const EngineUpdateItemHeader &h)
{
    std::vector<std::string> keys;
    for (const auto &item : h.cueItems) {
        keys.push_back(item.key);
    }
    return keys;
}

}  // namespace

int main()
{
    // R equals E: nothing, with or without a baseline.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {{"P", 0}}, 3)};
        w.engine = {en("e1", "Music/A.mp3", {{"P", 0}}, 3)};
        w.rekordboxPlaylists = {{"P", false, 1}};
        w.enginePlaylists = {{"P", false, 1}};
        w.importKey = {{"e1", 14204}};
        const auto base = baselineOf(w);
        const auto p = plan(w, base);
        assert(p.empty());
        assert(p.hasBaseline && p.baselineSequence == 14204 && p.currentSequence == 15132);
        assert(p.engineOwnKept.empty());
        assert(plan(w, std::nullopt).empty());
        std::cout << "level OK\n";
    }

    // B equals E, rekordbox added a track: add, checked.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3")};
        w.engine = {en("e1", "Music/A.mp3")};
        const auto base = baselineOf(w);
        w.rekordbox.push_back(rb("2", "Music/B.mp3"));
        const auto p = plan(w, base);
        assert(p.tracksToAdd.size() == 1);
        const auto &add = p.tracksToAdd[0];
        assert(add.header.key == "track:music/b.mp3");
        assert(add.header.checkedByDefault && !add.header.conflict);
        assert(add.header.reason == EngineUpdateReason::RekordboxAdded);
        assert(add.header.reasonText == "rekordbox added this track" + Since);
        assert(add.stickRelativePath == "Music/B.mp3" && add.rekordbox.sourceId == "2");
        assert(add.header.dependsOn.empty());
        assert(p.conflicts.empty() && p.tracksToRemove.empty() && p.membership.empty());
        std::cout << "track added in rekordbox OK\n";
    }

    // B equals E, rekordbox removed a track: removal, checked.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3"), rb("2", "Music/B.mp3", {{"P", 0}})};
        w.engine = {en("e1", "Music/A.mp3"), en("e2", "Music/B.mp3", {{"P", 0}})};
        w.rekordboxPlaylists = {{"P", false, 1}};
        w.enginePlaylists = {{"P", false, 1}};
        const auto base = baselineOf(w);
        w.rekordbox.pop_back();
        const auto p = plan(w, base);
        assert(p.tracksToRemove.size() == 1);
        const auto &rm = p.tracksToRemove[0];
        assert(rm.header.key == "track:music/b.mp3" && rm.header.checkedByDefault && !rm.header.conflict);
        assert(rm.header.reason == EngineUpdateReason::RekordboxRemoved);
        assert(rm.header.reasonText == "rekordbox removed this track" + Since);
        assert(rm.engine.sourceId == "e2" && rm.playlistCount == 1);
        // Its membership goes with the row, not as a row of its own.
        assert(p.membership.empty() && p.conflicts.empty());
        std::cout << "track removed in rekordbox, with a baseline OK\n";
    }

    // No baseline: an imported Engine row rekordbox does not list is a
    // conflict, one with pdbImportKey 0 is Engine's own; additions checked.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3"), rb("4", "Music/D.mp3")};
        w.engine = {en("e1", "Music/A.mp3"), en("e2", "Music/B.mp3"), en("e3", "Music/C.mp3")};
        w.importKey = {{"e1", 14204}, {"e2", 14204}, {"e3", 0}};
        const auto p = plan(w, std::nullopt);
        assert(!p.hasBaseline);
        assert(p.tracksToRemove.empty());
        assert(p.conflicts.size() == 1);
        const auto &c = p.conflicts[0];
        assert(c.header.key == "track:music/b.mp3" && c.header.conflict && !c.header.checkedByDefault);
        assert(c.header.reason == EngineUpdateReason::NoBaselineImportedRow);
        assert(c.header.reasonText
               == "Engine imported this track from rekordbox, which no longer lists it; no earlier record says "
                  "whether it was removed there");
        assert(c.rekordboxChoice.size() == 1 && c.engineChoice.empty());
        assert(std::get<TrackToRemove>(c.rekordboxChoice[0]).engine.sourceId == "e2");
        assert(p.engineOwnKept.size() == 1);
        assert(p.engineOwnKept[0].header.key == "track:music/c.mp3");
        assert(p.engineOwnKept[0].header.reason == EngineUpdateReason::NoBaselineEngineOnly);
        assert(p.engineOwnKept[0].header.reasonText == "Engine's own track: no rekordbox import put it there");
        assert(p.tracksToAdd.size() == 1 && p.tracksToAdd[0].header.key == "track:music/d.mp3");
        assert(p.tracksToAdd[0].header.checkedByDefault);
        assert(p.tracksToAdd[0].header.reason == EngineUpdateReason::NoBaselineAddition);
        assert(p.tracksToAdd[0].header.reasonText
               == "No earlier record of this stick: rekordbox lists this track and Engine does not");
        std::cout << "no baseline: removal conflict, Engine's own kept, addition checked OK\n";
    }

    // B equals R, E differs: Engine's own, kept, nothing to write.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3")};
        w.engine = {en("e1", "Music/A.mp3")};
        const auto base = baselineOf(w);
        w.engine.push_back(en("e9", "Music/E.mp3"));
        w.importKey = {{"e9", 14204}};  // imported or not, B decides
        const auto p = plan(w, base);
        assert(p.empty());
        assert(p.engineOwnKept.size() == 1);
        assert(p.engineOwnKept[0].header.key == "track:music/e.mp3");
        assert(p.engineOwnKept[0].header.reason == EngineUpdateReason::EngineOwn);
        assert(p.engineOwnKept[0].header.reasonText == "Engine added this track after Seabass last recorded the stick");
        assert(p.engineOwnKept[0].engine.sourceId == "e9");
        std::cout << "Engine's own kept OK\n";
    }

    // A playlist renamed in rekordbox, by its id; a renamed folder takes
    // its playlist along without a row of its own.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {{"Old", 0}, {"F/X", 0}})};
        w.engine = {en("e1", "Music/A.mp3", {{"Old", 0}, {"F/X", 0}})};
        w.rekordboxPlaylists = {{"Old", false, 1}, {"F", true, 2}, {"F/X", false, 3}};
        w.enginePlaylists = {{"Old", false, 1}, {"F", true, 1}, {"F/X", false, 1}};
        const auto base = baselineOf(w);
        w.rekordbox = {rb("1", "Music/A.mp3", {{"New", 0}, {"G/X", 0}})};
        w.rekordboxPlaylists = {{"New", false, 1}, {"G", true, 2}, {"G/X", false, 3}};
        const auto p = plan(w, base);
        assert(p.playlistsToRename.size() == 2);
        assert(p.playlistsToRename[0].header.key == "playlist:1");
        assert(p.playlistsToRename[0].fromPath == "Old" && p.playlistsToRename[0].toPath == "New");
        assert(p.playlistsToRename[0].header.checkedByDefault);
        assert(p.playlistsToRename[0].header.reason == EngineUpdateReason::RekordboxRenamed);
        assert(p.playlistsToRename[0].header.reasonText
               == "rekordbox renamed this playlist from \"Old\" to \"New\"" + Since);
        assert(p.playlistsToRename[1].header.key == "playlist:2");
        assert(p.playlistsToRename[1].fromPath == "F" && p.playlistsToRename[1].toPath == "G");
        assert(p.playlistsToCreate.empty() && p.playlistsToDelete.empty() && p.membership.empty());
        assert(p.conflicts.empty() && p.engineOwnKept.empty());
        World after = w;
        apply(after, p);
        assert(engineOrder(after, "New") == Deps{"music/a.mp3"});
        assert(engineOrder(after, "G/X") == Deps{"music/a.mp3"});
        assert(plan(after, base).empty());
        std::cout << "playlist renamed by id OK\n";
    }

    // An Engine path two playlists share is a conflict, nothing edited.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {{"Dup", 0}})};
        w.engine = {en("e1", "Music/A.mp3")};
        w.rekordboxPlaylists = {{"Dup", false, 7}};
        w.enginePlaylists = {{"Dup", false, 2}, {"Dup", false, 2}};
        const auto p = plan(w, std::nullopt);
        assert(p.conflicts.size() == 1);
        const auto &c = p.conflicts[0];
        assert(c.header.key == "playlist:7" && c.header.conflict);
        assert(c.header.reason == EngineUpdateReason::EnginePathAmbiguous);
        assert(c.header.reasonText == "Engine has 2 playlists at \"Dup\": rename one in Engine DJ first");
        assert(c.rekordboxChoice.empty() && c.engineChoice.empty());
        assert(p.membership.empty() && p.playlistsToCreate.empty() && p.engineOwnKept.empty());
        std::cout << "ambiguous Engine path conflict OK\n";
    }

    // Membership adds keep rekordbox's order through the "after" anchor,
    // the start of the playlist included.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {{"P", 0}}), rb("2", "Music/B.mp3"), rb("3", "Music/C.mp3", {{"P", 1}}),
                       rb("4", "Music/D.mp3")};
        w.engine = {en("e1", "Music/A.mp3", {{"P", 0}}), en("e2", "Music/B.mp3"), en("e3", "Music/C.mp3", {{"P", 1}}),
                    en("e4", "Music/D.mp3")};
        w.rekordboxPlaylists = {{"P", false, 1}};
        w.enginePlaylists = {{"P", false, 1}};
        const auto base = baselineOf(w);
        w.rekordbox = {rb("1", "Music/A.mp3", {{"P", 1}}), rb("2", "Music/B.mp3", {{"P", 2}}),
                       rb("3", "Music/C.mp3", {{"P", 3}}), rb("4", "Music/D.mp3", {{"P", 0}})};
        const auto p = plan(w, base);
        assert(p.membership.size() == 2);
        const auto &d = p.membership[0];
        assert(d.kind == MembershipEdit::Kind::Add && d.pathKey == "music/d.mp3" && d.afterPathKey.empty());
        assert(d.header.key == "member:1:music/d.mp3" && d.playlistPath == "P" && d.header.checkedByDefault);
        assert(d.header.reason == EngineUpdateReason::RekordboxAdded);
        assert(d.header.reasonText == "rekordbox added this track to \"P\"" + Since);
        assert(d.header.dependsOn.empty());
        const auto &b = p.membership[1];
        assert(b.kind == MembershipEdit::Kind::Add && b.pathKey == "music/b.mp3" && b.afterPathKey == "music/a.mp3");
        assert(p.conflicts.empty());
        World after = w;
        apply(after, p);
        assert((engineOrder(after, "P") == Deps{"music/d.mp3", "music/a.mp3", "music/b.mp3", "music/c.mp3"}));
        assert(plan(after, base).empty());
        std::cout << "membership order with the anchor OK\n";
    }

    // The same members in a new order: the moved entry is removed and
    // added again at its place, one item.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {{"P", 0}}), rb("2", "Music/B.mp3", {{"P", 1}}),
                       rb("3", "Music/C.mp3", {{"P", 2}})};
        w.engine = {en("e1", "Music/A.mp3", {{"P", 0}}), en("e2", "Music/B.mp3", {{"P", 1}}),
                    en("e3", "Music/C.mp3", {{"P", 2}})};
        w.rekordboxPlaylists = {{"P", false, 1}};
        w.enginePlaylists = {{"P", false, 1}};
        const auto base = baselineOf(w);
        w.rekordbox = {rb("1", "Music/A.mp3", {{"P", 1}}), rb("2", "Music/B.mp3", {{"P", 2}}),
                       rb("3", "Music/C.mp3", {{"P", 0}})};
        const auto p = plan(w, base);
        assert(p.membership.size() == 2);
        assert(p.membership[0].kind == MembershipEdit::Kind::Remove && p.membership[0].pathKey == "music/c.mp3");
        assert(p.membership[0].track.sourceId == "e3");
        assert(p.membership[1].kind == MembershipEdit::Kind::Add && p.membership[1].pathKey == "music/c.mp3");
        assert(p.membership[1].afterPathKey.empty());
        for (const auto &m : p.membership) {
            assert(m.header.key == "member:1:music/c.mp3" && m.header.checkedByDefault);
            assert(m.header.reason == EngineUpdateReason::RekordboxMoved);
            assert(m.header.reasonText == "rekordbox moved this track within \"P\"" + Since);
        }
        assert(p.conflicts.empty());
        World after = w;
        apply(after, p);
        assert((engineOrder(after, "P") == Deps{"music/c.mp3", "music/a.mp3", "music/b.mp3"}));
        assert(plan(after, base).empty());

        // Without a baseline the same reorder is a conflict.
        const auto q = plan(w, std::nullopt);
        assert(q.membership.empty() && q.conflicts.size() == 1);
        assert(q.conflicts[0].header.reason == EngineUpdateReason::NoBaselineOrder);
        assert(q.conflicts[0].header.key == "member:1:music/c.mp3");
        assert(q.conflicts[0].rekordboxChoice.size() == 2);
        std::cout << "same-set reorder OK\n";
    }

    // A new track's membership depends on the track's add, a new
    // playlist's on its create.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {{"P", 0}}), rb("2", "Music/N.mp3", {{"P", 1}, {"Q", 0}})};
        w.engine = {en("e1", "Music/A.mp3", {{"P", 0}})};
        w.rekordboxPlaylists = {{"P", false, 1}, {"Q", false, 2}};
        w.enginePlaylists = {{"P", false, 1}};
        w.importKey = {{"e1", 14204}};
        const auto p = plan(w, std::nullopt);
        assert(p.tracksToAdd.size() == 1 && p.tracksToAdd[0].header.key == "track:music/n.mp3");
        assert(p.playlistsToCreate.size() == 1);
        assert(p.playlistsToCreate[0].header.key == "playlist:2" && p.playlistsToCreate[0].path == "Q");
        assert(p.playlistsToCreate[0].header.reasonText
               == "No earlier record of this stick: rekordbox has this playlist and Engine does not");
        assert(p.membership.size() == 2);
        assert(p.membership[0].playlistPath == "P" && p.membership[0].afterPathKey == "music/a.mp3");
        assert((p.membership[0].header.dependsOn == Deps{"track:music/n.mp3"}));
        assert(p.membership[0].header.reasonText
               == "No earlier record of this playlist: rekordbox's \"P\" holds this track and Engine's does not");
        assert(p.membership[1].playlistPath == "Q" && p.membership[1].afterPathKey.empty());
        assert((p.membership[1].header.dependsOn == Deps{"playlist:2", "track:music/n.mp3"}));
        World after = w;
        apply(after, p);
        assert(plan(after, std::nullopt).empty());
        std::cout << "new track's membership depends on its add OK\n";
    }

    // Ratings: B equals E and R differs goes to Engine; B equals R is
    // Engine's own; all three differ is a conflict.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {}, 3), rb("2", "Music/B.mp3", {}, 3), rb("3", "Music/C.mp3", {}, 3)};
        w.engine = {en("e1", "Music/A.mp3", {}, 3), en("e2", "Music/B.mp3", {}, 3), en("e3", "Music/C.mp3", {}, 3)};
        const auto base = baselineOf(w);
        w.rekordbox[0].rating = 5;  // rekordbox changed A
        w.engine[1].rating = 4;     // Engine changed B
        w.rekordbox[2].rating = 5;  // both changed C
        w.engine[2].rating = 2;
        const auto p = plan(w, base);
        assert(p.metadataToEngine.size() == 1);
        const auto &m = p.metadataToEngine[0];
        assert(m.header.key == "rating:music/a.mp3" && m.header.checkedByDefault);
        assert(m.field == MetadataEdit::Field::Rating && m.direction == MetadataEdit::Direction::ToEngine);
        assert(m.rating == 5 && m.engine.sourceId == "e1");
        assert(m.header.reason == EngineUpdateReason::RekordboxChanged);
        assert(m.header.reasonText == "rekordbox changed the rating from 3 stars to 5 stars" + Since);
        assert(p.engineOwnKept.size() == 1 && p.engineOwnKept[0].header.key == "rating:music/b.mp3");
        assert(p.engineOwnKept[0].header.reasonText
               == "Engine changed the rating from 3 stars to 4 stars after Seabass last recorded the stick");
        assert(p.conflicts.size() == 1 && p.conflicts[0].header.key == "rating:music/c.mp3");
        assert(p.conflicts[0].header.reason == EngineUpdateReason::BothChanged);
        assert(p.conflicts[0].header.reasonText
               == "Both sides changed the rating since Seabass last recorded the stick (rekordbox 5 stars, Engine "
                  "2 stars, recorded 3 stars)");
        World after = w;
        apply(after, p, true);
        assert(after.engine[0].rating == 5 && after.engine[2].rating == 5 && after.engine[1].rating == 4);
        assert(plan(after, base).empty());
        std::cout << "rating change to Engine, Engine's own, both changed OK\n";
    }

    // No baseline: differing ratings are a conflict.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {}, 5)};
        w.engine = {en("e1", "Music/A.mp3", {}, 3)};
        const auto p = plan(w, std::nullopt);
        assert(p.metadataToEngine.empty() && p.conflicts.size() == 1);
        const auto &c = p.conflicts[0];
        assert(c.header.key == "rating:music/a.mp3" && c.header.conflict && !c.header.checkedByDefault);
        assert(c.header.reason == EngineUpdateReason::NoBaselineValuesDiffer);
        assert(c.header.reasonText
               == "Ratings differ (rekordbox 5 stars, Engine 3 stars) and no earlier record says which side changed");
        assert(c.rekordboxSide == "rekordbox: 5 stars" && c.engineSide == "Engine: 3 stars");
        assert(std::get<MetadataEdit>(c.rekordboxChoice[0]).rating == 5 && c.engineChoice.empty());
        std::cout << "rating conflict without a baseline OK\n";
    }

    // B equals E, R lacks a rating B holds: Seabass's goes back onto
    // rekordbox, rekordbox's own removal goes to Engine, unknown asks.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {}, 4), rb("2", "Music/B.mp3", {}, 4), rb("3", "Music/C.mp3", {}, 4)};
        w.engine = {en("e1", "Music/A.mp3", {}, 4), en("e2", "Music/B.mp3", {}, 4), en("e3", "Music/C.mp3", {}, 4)};
        auto base = baselineOf(w);
        base.tracks[0].ratingOrigin = ValueOrigin::Seabass;
        base.tracks[1].ratingOrigin = ValueOrigin::Rekordbox;
        base.tracks[2].ratingOrigin = ValueOrigin::Unknown;
        for (auto &t : w.rekordbox) {
            t.rating = std::nullopt;
        }
        const auto p = plan(w, base);
        assert(p.restoresToRekordbox.size() == 1);
        const auto &r = p.restoresToRekordbox[0];
        assert(r.header.key == "rating:music/a.mp3" && r.header.checkedByDefault);
        assert(r.direction == MetadataEdit::Direction::ToRekordbox && r.rating == 4 && r.rekordbox.sourceId == "1");
        assert(r.header.reason == EngineUpdateReason::ExportDropped);
        assert(r.header.reasonText == "rekordbox's export dropped the rating Seabass had written (4 stars); it goes back");
        assert(p.metadataToEngine.size() == 1 && p.metadataToEngine[0].header.key == "rating:music/b.mp3");
        assert(!p.metadataToEngine[0].rating);
        assert(p.metadataToEngine[0].header.reasonText == "rekordbox cleared the rating (4 stars)" + Since);
        assert(p.conflicts.size() == 1 && p.conflicts[0].header.key == "rating:music/c.mp3");
        assert(p.conflicts[0].header.reason == EngineUpdateReason::OriginUnknown);
        const auto &back = std::get<MetadataEdit>(p.conflicts[0].engineChoice[0]);
        assert(back.direction == MetadataEdit::Direction::ToRekordbox && back.rating == 4);
        World after = w;
        apply(after, p);
        assert(after.rekordbox[0].rating == 4 && !after.engine[1].rating);
        std::cout << "rating restore onto rekordbox OK\n";
    }

    // A declined item stays out while rekordbox's state of it is the one
    // recorded, and resurfaces once rekordbox changes it. A row depending
    // on a declined one stays out with it.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {{"P", 0}}, 3)};
        w.engine = {en("e1", "Music/A.mp3", {{"P", 0}}, 3)};
        w.rekordboxPlaylists = {{"P", false, 1}};
        w.enginePlaylists = {{"P", false, 1}};
        auto base = baselineOf(w);
        w.rekordbox[0].rating = 5;
        w.rekordbox.push_back(rb("2", "Music/B.mp3", {{"P", 1}}));
        const auto first = plan(w, base);
        assert(first.metadataToEngine.size() == 1 && first.tracksToAdd.size() == 1 && first.membership.size() == 1);
        assert(first.metadataToEngine[0].header.rekordboxState == "a2458be5bd5dcd91");  // FNV-1a of "rating:5"
        base.declined["rating:music/a.mp3"] = "a2458be5bd5dcd91";
        base.declined["track:music/b.mp3"] = first.tracksToAdd[0].header.rekordboxState;
        const auto second = plan(w, base);
        assert(second.empty());
        assert((second.declinedSuppressed == Deps{"member:1:music/b.mp3", "rating:music/a.mp3", "track:music/b.mp3"}));
        w.rekordbox[0].rating = 4;
        const auto third = plan(w, base);
        assert(third.metadataToEngine.size() == 1 && third.metadataToEngine[0].rating == 4);
        assert(third.metadataToEngine[0].header.rekordboxState == "a2458ae5bd5dcbde");  // "rating:4"
        assert(third.tracksToAdd.empty() && third.membership.empty());
        std::cout << "declined item suppressed, then resurfacing OK\n";
    }

    // Streaming rows are not tracks on the stick: never planned.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3")};
        w.engine = {en("e1", "Music/A.mp3"), en("e2", "Streaming/X.flac", {{"P", 0}})};
        w.engine[1].streamingSource = "TIDAL";
        w.importKey = {{"e1", 14204}, {"e2", 14204}};
        const auto p = plan(w, std::nullopt);
        assert(p.empty() && p.engineOwnKept.empty());
        std::cout << "streaming rows ignored OK\n";
    }

    // Two Engine rows for one file: one conflict, nothing else for it.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {}, 5)};
        w.engine = {en("e1", "Music/A.mp3", {}, 3), en("e2", "Music/A.mp3", {}, 3)};
        const auto p = plan(w, std::nullopt);
        assert(p.conflicts.size() == 1);
        const auto &c = p.conflicts[0];
        assert(c.header.key == "track:music/a.mp3" && c.header.reason == EngineUpdateReason::DuplicateEngineRows);
        assert(c.header.reasonText == "Engine lists this file 2 times: Clean Up first");
        assert(c.rekordboxChoice.empty() && c.engineChoice.empty());
        assert(p.tracksToAdd.empty() && p.metadataToEngine.empty() && p.tracksToRemove.empty());
        std::cout << "two Engine rows for one file OK\n";
    }

    // A file rekordbox lists that is not on the stick: refused, said so,
    // and its membership with it.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {{"P", 0}}), rb("2", "Music/Gone.mp3", {{"P", 1}})};
        w.engine = {en("e1", "Music/A.mp3", {{"P", 0}})};
        w.rekordboxPlaylists = {{"P", false, 1}};
        w.enginePlaylists = {{"P", false, 1}};
        w.missingFiles = {"Music/Gone.mp3"};
        const auto p = plan(w, std::nullopt);
        assert(p.tracksToAdd.empty() && p.membership.empty());
        assert(p.notAdded.size() == 1);
        assert(p.notAdded[0].header.key == "track:music/gone.mp3" && !p.notAdded[0].header.checkedByDefault);
        assert(p.notAdded[0].header.reason == EngineUpdateReason::FileNotOnStick);
        assert(p.notAdded[0].header.reasonText
               == "rekordbox lists this file and the stick does not have it: nothing to add");
        assert(p.empty());
        std::cout << "file not on the stick refused OK\n";
    }

    // No baseline: a member only Engine's copy holds is a conflict; a
    // copy of the same song rekordbox no longer lists makes room for the
    // one rekordbox's playlist holds.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {{"P", 0}}), rb("2", "Music/B.mp3"), rb("3", "Music/S2.mp3", {{"P", 1}})};
        w.rekordbox[2].title = "Song";
        w.engine = {en("e1", "Music/A.mp3", {{"P", 0}}), en("e2", "Music/B.mp3", {{"P", 1}}),
                    en("e3", "Music/S2.mp3"), en("e4", "Music/S1.mp3", {{"P", 2}})};
        w.engine[2].title = "Song";
        w.engine[3].title = "Song";
        w.rekordboxPlaylists = {{"P", false, 1}};
        w.enginePlaylists = {{"P", false, 1}};
        w.importKey = {{"e1", 1}, {"e2", 1}, {"e3", 1}, {"e4", 0}};
        const auto p = plan(w, std::nullopt);
        assert(p.conflicts.size() == 1);
        assert(p.conflicts[0].header.key == "member:1:music/b.mp3");
        assert(p.conflicts[0].header.reason == EngineUpdateReason::NoBaselineEngineMember);
        assert(p.conflicts[0].header.reasonText
               == "Engine's \"P\" holds this track and rekordbox's does not; no earlier record says which side "
                  "changed it");
        assert(p.membership.size() == 2);
        assert(p.membership[0].kind == MembershipEdit::Kind::Remove && p.membership[0].pathKey == "music/s1.mp3");
        assert(p.membership[0].header.key == "member:1:music/s2.mp3");
        assert(p.membership[0].header.reason == EngineUpdateReason::SameSongCopy);
        assert(p.membership[1].kind == MembershipEdit::Kind::Add && p.membership[1].pathKey == "music/s2.mp3");
        assert(p.membership[1].afterPathKey == "music/a.mp3");
        World after = w;
        apply(after, p, true);
        assert((engineOrder(after, "P") == Deps{"music/a.mp3", "music/s2.mp3"}));
        assert(plan(after, std::nullopt).empty());
        std::cout << "Engine-only member conflict and same-song copy swap OK\n";
    }

    // Idempotence over every section at once: plan, apply, plan again.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {{"Keep", 0}, {"Gone", 0}}, 2), rb("2", "Music/B.mp3", {{"Keep", 1}}),
                       rb("3", "Music/C.mp3", {{"Keep", 2}, {"Old", 0}}), rb("4", "Music/R.mp3", {{"Keep", 3}})};
        w.engine = {en("e1", "Music/A.mp3", {{"Keep", 0}, {"Gone", 0}}, 2), en("e2", "Music/B.mp3", {{"Keep", 1}}),
                    en("e3", "Music/C.mp3", {{"Keep", 2}, {"Old", 0}}), en("e4", "Music/R.mp3", {{"Keep", 3}})};
        w.rekordboxPlaylists = {{"Keep", false, 1}, {"Gone", false, 2}, {"Old", false, 3}};
        w.enginePlaylists = {{"Keep", false, 1}, {"Gone", false, 1}, {"Old", false, 1}};
        const auto base = baselineOf(w);
        // rekordbox: rate A, drop R, add N to Keep and a new folder list,
        // reorder Keep, rename Old, delete Gone.
        w.rekordbox = {rb("1", "Music/A.mp3", {{"Keep", 2}}, 4), rb("2", "Music/B.mp3", {{"Keep", 0}}),
                       rb("3", "Music/C.mp3", {{"Keep", 1}, {"Renamed", 0}}),
                       rb("5", "Music/N.mp3", {{"Keep", 3}, {"Crate/Fresh", 0}})};
        w.rekordboxPlaylists = {{"Keep", false, 1}, {"Renamed", false, 3}, {"Crate", true, 4}, {"Crate/Fresh", false, 5}};
        const auto p = plan(w, base);
        assert(p.conflicts.empty());
        assert(p.tracksToAdd.size() == 1 && p.tracksToRemove.size() == 1);
        assert(p.playlistsToCreate.size() == 2 && p.playlistsToCreate[0].path == "Crate");
        assert((p.playlistsToCreate[1].header.dependsOn == Deps{"playlist:4"}));
        assert(p.playlistsToRename.size() == 1 && p.playlistsToDelete.size() == 1);
        assert(p.playlistsToDelete[0].path == "Gone" && p.playlistsToDelete[0].engineMembers == 1);
        assert(p.metadataToEngine.size() == 1);
        World after = w;
        apply(after, p);
        assert((engineOrder(after, "Keep") == Deps{"music/b.mp3", "music/c.mp3", "music/a.mp3", "music/n.mp3"}));
        assert((engineOrder(after, "Crate/Fresh") == Deps{"music/n.mp3"}));
        const auto again = plan(after, base);
        assert(again.empty());
        // And recorded afresh, as the save will: still nothing.
        assert(plan(after, baselineFrom(after.rekordbox, after.rekordboxPlaylists, 15132, stickRelative, lowerKey)).empty());
        std::cout << "idempotence OK\n";
    }

    // Step 2 correction: without a baseline only two values that are both
    // set and differ are a conflict. A value only Engine has is Engine's
    // own, kept (removing it would be destructive and unattributed); one
    // only rekordbox has is an addition, checked.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3"), rb("2", "Music/B.mp3"), rb("3", "Music/C.mp3", {}, 5)};
        w.engine = {en("e1", "Music/A.mp3"), en("e2", "Music/B.mp3", {}, 4), en("e3", "Music/C.mp3")};
        w.rekordbox[1].comment = "rekordbox note";
        w.engine[0].comment = "Engine note";
        w.engine[1].comment = "another note";
        const auto p = plan(w, std::nullopt);
        // B: two comments, both set: the one conflict.
        assert(p.conflicts.size() == 1);
        assert(p.conflicts[0].header.key == "comment:music/b.mp3");
        assert(p.conflicts[0].header.reason == EngineUpdateReason::NoBaselineValuesDiffer);
        assert(p.conflicts[0].header.reasonText == "Comments differ and no earlier record says which side changed");
        // A's comment and B's rating: Engine's own, kept.
        assert(p.engineOwnKept.size() == 2);
        assert(p.engineOwnKept[0].header.key == "comment:music/a.mp3");
        assert(p.engineOwnKept[0].header.reason == EngineUpdateReason::NoBaselineEngineValue);
        assert(p.engineOwnKept[0].header.reasonText
               == "Engine's own comment: rekordbox has none, and with no earlier record nothing is removed");
        assert(p.engineOwnKept[1].header.key == "rating:music/b.mp3");
        assert(p.engineOwnKept[1].header.reason == EngineUpdateReason::NoBaselineEngineValue);
        assert(p.engineOwnKept[1].header.reasonText
               == "Engine's own rating (4 stars): rekordbox has none, and with no earlier record nothing is removed");
        // C's rating: only rekordbox has one, it goes to Engine.
        assert(p.metadataToEngine.size() == 1);
        const auto &m = p.metadataToEngine[0];
        assert(m.header.key == "rating:music/c.mp3" && m.header.checkedByDefault && m.rating == 5);
        assert(m.header.reason == EngineUpdateReason::NoBaselineAddition);
        assert(m.header.reasonText
               == "No earlier record of this stick: rekordbox has a rating (5 stars) and Engine has none");
        World after = w;
        apply(after, p);
        const auto again = plan(after, std::nullopt);
        assert(again.conflicts.size() == 1 && again.metadataToEngine.empty() && again.engineOwnKept.size() == 2);
        std::cout << "no baseline: a value only Engine has is kept, two values a conflict OK\n";
    }

    // Cues, rule A: a cue rekordbox added goes to Engine, one row per
    // track, the SyncPlan written unchanged.
    {
        World w = cuePair({hot(1, 10000)}, {hot(1, 10000)});
        const auto base = baselineOf(w);
        w.rekordbox[0].cues.push_back(hot(2, 20000));
        const auto p = plan(w, base);
        assert(p.cuesToEngine.size() == 1 && p.cuesToRekordbox.empty() && p.conflicts.empty());
        assert(p.engineOwnKept.empty());
        const auto &c = p.cuesToEngine[0];
        assert(c.header.key == "cue:hot:2:music/a.mp3" && c.pathKey == "music/a.mp3");
        assert(c.header.checkedByDefault && !c.header.conflict);
        assert(c.header.reason == EngineUpdateReason::RekordboxAdded);
        assert(c.header.reasonText == "rekordbox set pad 2 (0:20.000)" + Since);
        assert(c.header.rekordboxState == engineUpdateStateHash("cue:hot:2:cue@20000"));
        assert((itemKeys(c.header) == Deps{"cue:hot:2:music/a.mp3"}));
        assert(c.plan.direction == SyncPlan::Direction::ToB);
        assert(c.plan.match.trackA.sourceId == "1" && c.plan.match.trackB.sourceId == "e1");
        assert(sameCueList(c.plan.cuesToApply, {hot(1, 10000), hot(2, 20000)}));
        assert(c.plan.positionToleranceMs == 250.0);
        assert(p.onlyCues());
        World after = w;
        apply(after, p);
        assert(plan(after, base).empty());
        std::cout << "cue added in rekordbox goes to Engine OK\n";
    }

    // Rule A: a pad moved within half a beat is nothing; beyond it, it goes.
    {
        World w = cuePair({hot(1, 10000)}, {hot(1, 10000)});
        const auto base = baselineOf(w);
        w.rekordbox[0].cues = {hot(1, 10200)};  // 200 ms, under 250
        assert(plan(w, base).empty());
        w.rekordbox[0].cues = {hot(1, 10300)};  // 300 ms
        const auto p = plan(w, base);
        assert(p.cuesToEngine.size() == 1);
        const auto &c = p.cuesToEngine[0];
        assert(c.header.key == "cue:hot:1:music/a.mp3");
        assert(c.header.reason == EngineUpdateReason::RekordboxChanged);
        assert(c.header.reasonText == "rekordbox moved pad 1 from 0:10.000 to 0:10.300" + Since);
        assert(sameCueList(c.plan.cuesToApply, {hot(1, 10300)}));
        std::cout << "cue moved within and beyond tolerance OK\n";
    }

    // Rule C: a pad moved on both sides is a conflict naming both places.
    {
        World w = cuePair({hot(1, 10000), hot(3, 40000)}, {hot(1, 10000), hot(3, 40000)});
        const auto base = baselineOf(w);
        w.rekordbox[0].cues[0].positionMs = 20000;
        w.engine[0].cues[0].positionMs = 30000;
        const auto p = plan(w, base);
        assert(p.cuesToEngine.empty() && p.conflicts.size() == 1);
        const auto &c = p.conflicts[0];
        assert(c.header.key == "cue:hot:1:music/a.mp3" && c.header.conflict && !c.header.checkedByDefault);
        assert(c.header.reason == EngineUpdateReason::BothChanged);
        assert(c.header.reasonText
               == "Pad 1: rekordbox 0:20.000, Engine 0:30.000; both sides changed it since Seabass last recorded the "
                  "stick (recorded 0:10.000)");
        assert(c.rekordboxSide == "Follow rekordbox" && c.engineSide == "Keep Engine's cues");
        assert(c.rekordboxChoice.size() == 1 && c.engineChoice.empty());
        const auto &choice = std::get<CueEdit>(c.rekordboxChoice[0]);
        assert(choice.plan.direction == SyncPlan::Direction::ToB);
        assert(sameCueList(choice.plan.cuesToApply, {hot(1, 20000), hot(3, 40000)}));
        assert(p.onlyCues());
        World after = w;
        apply(after, p, true);
        assert(plan(after, base).empty());
        std::cout << "cue moved on both sides is a conflict OK\n";
    }

    // Rule B: cues rekordbox's export dropped. Seabass wrote them: they go
    // back onto rekordbox.
    {
        World w = cuePair({hot(1, 10000), hot(3, 40000), mem(30000)},
                          {hot(1, 10000), hot(3, 40000), hot(2, 30000), mem(30000)});
        auto base = baselineOf(w);
        setOrigin(base, ValueOrigin::Seabass);
        w.rekordbox[0].cues = {hot(3, 40000)};
        const auto p = plan(w, base);
        assert(p.cuesToEngine.empty() && p.conflicts.empty() && p.engineOwnKept.empty());
        assert(p.cuesToRekordbox.size() == 1);
        const auto &c = p.cuesToRekordbox[0];
        assert(c.header.key == "cue:hot:1:music/a.mp3" && c.header.checkedByDefault);
        assert((itemKeys(c.header) == Deps{"cue:hot:1:music/a.mp3", "cue:memory:30000:music/a.mp3"}));
        assert(c.header.reason == EngineUpdateReason::ExportDropped);
        assert(c.header.reasonText == "rekordbox's export dropped 2 cues Seabass had synced from Engine; they go back");
        assert(c.plan.direction == SyncPlan::Direction::ToA);
        assert(sameCueList(c.plan.cuesToApply, {hot(3, 40000), hot(1, 10000), mem(30000)}));
        assert(p.onlyCues());
        World after = w;
        apply(after, p);
        assert(plan(after, base).empty());
        std::cout << "Seabass's cues the export dropped go back OK\n";
    }

    // Rule B: rekordbox's own cue the DJ deleted there: removed from Engine.
    {
        World w = cuePair({hot(1, 10000), hot(2, 20000)}, {hot(1, 10000), hot(2, 20000)});
        auto base = baselineOf(w);
        setOrigin(base, ValueOrigin::Rekordbox);
        w.rekordbox[0].cues = {hot(1, 10000)};
        const auto p = plan(w, base);
        assert(p.cuesToEngine.size() == 1 && p.cuesToRekordbox.empty() && p.conflicts.empty());
        const auto &c = p.cuesToEngine[0];
        assert(c.header.key == "cue:hot:2:music/a.mp3" && c.header.checkedByDefault);
        assert(c.header.reason == EngineUpdateReason::RekordboxRemoved);
        assert(c.header.reasonText == "rekordbox cleared pad 2 (0:20.000)" + Since);
        assert(sameCueList(c.plan.cuesToApply, {hot(1, 10000)}));
        World after = w;
        apply(after, p);
        assert(plan(after, base).empty());
        std::cout << "rekordbox's own dropped cue removed from Engine OK\n";
    }

    // Rule B: nobody recorded who wrote it: a conflict, either way possible.
    {
        World w = cuePair({hot(1, 10000), hot(2, 20000)}, {hot(1, 10000), hot(2, 20000)});
        const auto base = baselineOf(w);  // a read: every origin Unknown
        w.rekordbox[0].cues = {hot(1, 10000)};
        const auto p = plan(w, base);
        assert(p.cuesToEngine.empty() && p.cuesToRekordbox.empty() && p.conflicts.size() == 1);
        const auto &c = p.conflicts[0];
        assert(c.header.key == "cue:hot:2:music/a.mp3" && !c.header.checkedByDefault);
        assert(c.header.reason == EngineUpdateReason::OriginUnknown);
        assert(c.header.reasonText
               == "rekordbox no longer has pad 2 (0:20.000), which Engine has, and nothing recorded whether Seabass "
                  "or rekordbox put it there");
        assert(c.engineSide == "Put Engine's cues back on rekordbox");
        assert(sameCueList(std::get<CueEdit>(c.rekordboxChoice[0]).plan.cuesToApply, {hot(1, 10000)}));
        const auto &back = std::get<CueEdit>(c.engineChoice[0]);
        assert(back.plan.direction == SyncPlan::Direction::ToA);
        assert(sameCueList(back.plan.cuesToApply, {hot(1, 10000), hot(2, 20000)}));
        std::cout << "dropped cue of unknown origin is a conflict OK\n";
    }

    // Rule D: Engine changed a pad, rekordbox did not: Engine's own, kept.
    {
        World w = cuePair({hot(1, 10000)}, {hot(1, 10000)});
        const auto base = baselineOf(w);
        w.engine[0].cues.push_back(hot(4, 50000));
        const auto p = plan(w, base);
        assert(p.empty());
        assert(p.engineOwnKept.size() == 1);
        const auto &k = p.engineOwnKept[0];
        assert(k.header.key == "cue:hot:4:music/a.mp3" && k.header.reason == EngineUpdateReason::EngineOwn);
        assert(k.header.reasonText == "Engine set pad 4 (0:50.000) after Seabass last recorded the stick");
        assert(k.engine.sourceId == "e1");
        std::cout << "Engine's own cue kept OK\n";
    }

    // Junk: a cue in the first second is not a cue under "Ignore cues at
    // 0:00"; with the preference off it is one.
    {
        World w = cuePair({hot(1, 10000)}, {hot(1, 10000)});
        const auto base = baselineOf(w);
        w.rekordbox[0].cues.push_back(hot(2, 500));
        assert(plan(w, base).empty());
        MatchingPolicy::set(2.0, 10.0, false);
        const auto p = plan(w, base);
        MatchingPolicy::reset();
        assert(p.cuesToEngine.size() == 1);
        assert(p.cuesToEngine[0].header.reasonText == "rekordbox set pad 2 (0:00.500)" + Since);
        std::cout << "junk cue at 0:00 ignored under the policy OK\n";
    }

    // A pad at a memory cue rekordbox dropped, within half a beat but on a
    // pad Engine DJ's import would not have given it: the
    // EngineMemoryOrHotCue question. On the import's own pad it is that
    // memory cue and goes; beyond half a beat it is a hot cue of Engine's.
    {
        World w = cuePair({hot(1, 10000), mem(30000)}, {hot(1, 10000), hot(5, 30100)});
        auto base = baselineOf(w);
        setOrigin(base, ValueOrigin::Rekordbox);
        w.rekordbox[0].cues = {hot(1, 10000)};
        const auto p = plan(w, base);
        assert(p.cuesToEngine.empty() && p.conflicts.size() == 1);
        const auto &c = p.conflicts[0];
        assert(c.header.key == "cue:memory:30000:music/a.mp3");
        assert(c.header.reason == EngineUpdateReason::EngineMemoryOrHotCue);
        assert(c.header.reasonText
               == "Engine pad 5 sits where rekordbox had the memory cue at 0:30.000, which it no longer has: that cue "
                  "on Engine, or a hot cue of Engine's own?");
        assert(c.engineChoice.empty() && c.engineSide == "Keep Engine's cues");
        const auto &choice = std::get<CueEdit>(c.rekordboxChoice[0]);
        assert(choice.plan.reason == SyncPlan::Reason::EngineMemoryOrHotCue);
        assert(sameCueList(choice.plan.cuesToApply, {hot(1, 10000)}));

        World onImportPad = w;
        onImportPad.engine[0].cues = {hot(1, 10000), hot(2, 30000)};
        const auto q = plan(onImportPad, base);
        assert(q.conflicts.empty() && q.cuesToEngine.size() == 1);
        assert(q.cuesToEngine[0].header.reasonText == "rekordbox removed the memory cue at 0:30.000" + Since);
        assert(sameCueList(q.cuesToEngine[0].plan.cuesToApply, {hot(1, 10000)}));

        World apart = w;
        apart.engine[0].cues = {hot(1, 10000), hot(5, 30400)};
        const auto f = plan(apart, base);
        assert(f.empty() && f.engineOwnKept.size() == 1);
        assert(f.engineOwnKept[0].header.key == "cue:hot:5:music/a.mp3");
        std::cout << "pad at a memory cue within half a beat asks OK\n";
    }

    // Rule E: no baseline, the cues are what SyncPlanner::plan decides for
    // the pair: a direction is a checked row, a choice a conflict with its
    // reason.
    {
        World w = cuePair({hot(1, 10000), hot(2, 20000)}, {});
        World other = cuePair({hot(1, 10000)}, {hot(1, 20000)}, "Music/B.mp3", "2");
        w.rekordbox.push_back(other.rekordbox[0]);
        w.engine.push_back(other.engine[0]);
        w.importKey["e2"] = 14204;
        const auto p = plan(w, std::nullopt);
        const SyncPlan a = SyncPlanner::plan(SyncMatch{w.rekordbox[0], w.engine[0]}, {}, {});
        const SyncPlan b = SyncPlanner::plan(SyncMatch{w.rekordbox[1], w.engine[1]}, {}, {});
        assert(a.direction == SyncPlan::Direction::ToB && b.needsChoice);
        assert(p.cuesToEngine.size() == 1 && p.conflicts.size() == 1);
        const auto &c = p.cuesToEngine[0];
        assert(c.header.key == "cue:hot:1:music/a.mp3" && c.header.checkedByDefault);
        assert((itemKeys(c.header) == Deps{"cue:hot:1:music/a.mp3", "cue:hot:2:music/a.mp3"}));
        assert(c.header.reason == EngineUpdateReason::NoBaselineCues);
        assert(c.header.reasonText
               == "No earlier record of this stick: rekordbox's cues go to Engine, as Sync Cue Points decides");
        assert(sameCueList(c.plan.cuesToApply, a.cuesToApply));
        assert(sameCueList(c.plan.cuesToApply, {hot(1, 10000), hot(2, 20000)}));
        const auto &k = p.conflicts[0];
        assert(k.header.key == "cue:hot:1:music/b.mp3" && k.header.reason == EngineUpdateReason::NoBaselineCues);
        assert(k.header.reasonText == b.reasonText);
        assert(k.header.reasonText == "Pad 1: rekordbox 0:10.000, Engine 0:20.000");
        assert(sameCueList(std::get<CueEdit>(k.rekordboxChoice[0]).plan.cuesToApply, {hot(1, 10000)}));
        const auto &theirs = std::get<CueEdit>(k.engineChoice[0]);
        assert(theirs.plan.direction == SyncPlan::Direction::ToA);
        assert(sameCueList(theirs.plan.cuesToApply, {hot(1, 20000)}));
        World after = w;
        apply(after, p, true);
        assert(plan(after, std::nullopt).empty());
        std::cout << "no baseline: cues as Sync Cue Points decides OK\n";
    }

    // Rule F: an added track's cues ride in its TrackToAdd; a file Engine
    // lists twice gets no cue rows.
    {
        World w = cuePair({hot(1, 10000)}, {hot(1, 10000)});
        const auto base = baselineOf(w);
        Track n = rb("2", "Music/N.mp3");
        n.cues = {hot(1, 5000)};
        w.rekordbox.push_back(n);
        w.engine[0].cues.clear();
        w.engine.push_back(w.engine[0]);
        w.engine[1].sourceId = "e9";
        const auto p = plan(w, base);
        assert(p.tracksToAdd.size() == 1 && p.tracksToAdd[0].rekordbox.cues.size() == 1);
        assert(p.cuesToEngine.empty() && p.cuesToRekordbox.empty());
        assert(p.conflicts.size() == 1 && p.conflicts[0].header.reason == EngineUpdateReason::DuplicateEngineRows);
        std::cout << "no cue rows for added tracks or duplicate rows OK\n";
    }

    // A declined cue item stays out, the track's other items still go, and
    // it resurfaces once rekordbox changes it.
    {
        World w = cuePair({hot(1, 10000)}, {hot(1, 10000)});
        auto base = baselineOf(w);
        w.rekordbox[0].cues.push_back(hot(2, 20000));
        const auto first = plan(w, base);
        assert(first.cuesToEngine.size() == 1);
        base.declined["cue:hot:2:music/a.mp3"] = first.cuesToEngine[0].header.rekordboxState;
        const auto second = plan(w, base);
        assert(second.empty());
        assert((second.declinedSuppressed == Deps{"cue:hot:2:music/a.mp3"}));
        w.rekordbox[0].cues.push_back(hot(3, 30000));
        const auto third = plan(w, base);
        assert(third.cuesToEngine.size() == 1);
        assert((itemKeys(third.cuesToEngine[0].header) == Deps{"cue:hot:3:music/a.mp3"}));
        assert(sameCueList(third.cuesToEngine[0].plan.cuesToApply, {hot(1, 10000), hot(3, 30000)}));
        assert((third.declinedSuppressed == Deps{"cue:hot:2:music/a.mp3"}));
        w.rekordbox[0].cues[1].positionMs = 25000;
        const auto fourth = plan(w, base);
        assert(fourth.cuesToEngine.size() == 1 && fourth.declinedSuppressed.empty());
        assert((itemKeys(fourth.cuesToEngine[0].header) == Deps{"cue:hot:2:music/a.mp3", "cue:hot:3:music/a.mp3"}));
        assert(fourth.cuesToEngine[0].header.rekordboxState == engineUpdateStateHash("cue:hot:2:cue@25000"));
        std::cout << "declined cue item suppressed, then resurfacing OK\n";
    }

    // Idempotence with cues: a pad moved, a memory cue added (a free pad
    // and the cue point on Engine), Seabass's cue restored, rekordbox's
    // own deletion carried over. Plan, apply, plan again is empty.
    {
        World w = cuePair({hot(1, 10000)}, {hot(1, 10000)});
        const std::vector<std::pair<std::string, std::string>> more = {
            {"Music/B.mp3", "2"}, {"Music/C.mp3", "3"}, {"Music/D.mp3", "4"}};
        for (const auto &[relative, id] : more) {
            World one = cuePair({hot(1, 10000), hot(2, 20000)}, {hot(1, 10000), hot(2, 20000)}, relative, id);
            w.rekordbox.push_back(one.rekordbox[0]);
            w.engine.push_back(one.engine[0]);
            w.importKey["e" + id] = 14204;
        }
        auto base = baselineOf(w);
        base.tracks[2].cues[1].origin = ValueOrigin::Seabass;    // C's pad 2
        base.tracks[3].cues[1].origin = ValueOrigin::Rekordbox;  // D's pad 2
        w.rekordbox[0].cues.push_back(mem(60000));                // A: memory cue added
        w.rekordbox[1].cues[0].positionMs = 12000;                // B: pad 1 moved
        w.rekordbox[2].cues.pop_back();                           // C: export dropped Seabass's pad 2
        w.rekordbox[3].cues.pop_back();                           // D: DJ deleted pad 2
        const auto p = plan(w, base);
        assert(p.conflicts.empty() && p.cuesToEngine.size() == 3 && p.cuesToRekordbox.size() == 1);
        assert(p.cuesToEngine[0].header.key == "cue:memory:60000:music/a.mp3");
        assert(p.cuesToEngine[0].header.reasonText == "rekordbox added the memory cue at 1:00.000" + Since);
        assert(sameCueList(p.cuesToEngine[0].plan.cuesToApply, {hot(1, 10000), hot(2, 60000), mem(60000)}));
        World after = w;
        apply(after, p);
        assert(plan(after, base).empty());
        assert(plan(after, baselineFrom(after.rekordbox, after.rekordboxPlaylists, 15132, stickRelative, lowerKey)).empty());
        std::cout << "idempotence with cues OK\n";
    }

    std::cout << "engine_update_planning_test: all OK\n";
    return 0;
}
