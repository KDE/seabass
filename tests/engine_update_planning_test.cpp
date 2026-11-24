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
#include <map>
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
                        } else if constexpr (std::is_same_v<T, AnalysisEdit>) {
                            // Never a planner's choice: raised beside the plan.
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

CuePoint hotLoop(int pad, double ms, double endMs)
{
    CuePoint c = hot(pad, ms);
    c.isLoop = true;
    c.loopEndMs = endMs;
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

// Every row of a proposal, as the page lists it: header and the row's
// edit, if it has one.
template<typename Fn>
void eachRow(const EngineUpdateProposal &p, Fn &&fn)
{
    const auto all = [&](const auto &rows) {
        for (const auto &row : rows) {
            fn(row.header);
        }
    };
    all(p.playlistsToCreate);
    all(p.tracksToAdd);
    all(p.playlistsToRename);
    all(p.membership);
    all(p.metadataToEngine);
    all(p.cuesToEngine);
    all(p.playlistsToDelete);
    all(p.tracksToRemove);
    all(p.restoresToRekordbox);
    all(p.cuesToRekordbox);
    all(p.conflicts);
    all(p.engineOwnKept);
}

// The record a save of the page makes (RekordboxExportSyncController's
// offered, applied and declined, then nextBaseline): every listed key is
// offered; the rows ticked by default are applied, except those under
// `unticked` (a key the DJ unticked, and every row depending on it, as
// the page unticks them); conflicts stay unanswered. The keys the DJ
// unticked are declined at their row's state.
RekordboxBaseline recordAfterSave(const RekordboxBaseline &previous, const World &now, const EngineUpdateProposal &p,
                                  const std::set<std::string> &unticked = {})
{
    std::set<std::string> offered(p.declinedSuppressed.begin(), p.declinedSuppressed.end());
    std::set<std::string> applied;
    std::map<std::string, std::string> declined;
    eachRow(p, [&](const EngineUpdateItemHeader &h) {
        offered.insert(h.key);
        for (const auto &item : h.cueItems) {
            offered.insert(item.key);
        }
    });
    std::set<std::string> off = unticked;
    for (bool grew = true; grew;) {
        grew = false;
        eachRow(p, [&](const EngineUpdateItemHeader &h) {
            for (const auto &dep : h.dependsOn) {
                if (off.count(dep) && off.insert(h.key).second) {
                    grew = true;
                }
            }
        });
    }
    eachRow(p, [&](const EngineUpdateItemHeader &h) {
        if (h.conflict || !h.checkedByDefault) {
            return;
        }
        if (unticked.count(h.key)) {
            declined[h.key] = h.rekordboxState;
            return;
        }
        if (off.count(h.key)) {
            return;
        }
        applied.insert(h.key);
        for (const auto &item : h.cueItems) {
            applied.insert(item.key);
        }
    });
    for (const auto &key : applied) {
        declined.erase(key);
    }
    return nextBaseline(&previous, now.rekordbox, now.rekordboxPlaylists, 15132, offered, applied, declined,
                        stickRelative, lowerKey);
}

const BaselinePlaylist *recorded(const RekordboxBaseline &b, std::uint32_t id)
{
    return b.findPlaylist(id);
}

bool hasPlaylist(const World &w, const std::string &path)
{
    return std::any_of(w.enginePlaylists.begin(), w.enginePlaylists.end(),
                       [&](const EnginePlaylistInfo &pl) { return pl.path == path; });
}

// The proposal as a save with `unticked` applies it: without the rows of
// those keys and every row that depends on one (the page's rule, as in
// recordAfterSave).
EngineUpdateProposal tickedOnly(const EngineUpdateProposal &p, const std::set<std::string> &unticked)
{
    std::set<std::string> off = unticked;
    for (bool grew = true; grew;) {
        grew = false;
        eachRow(p, [&](const EngineUpdateItemHeader &h) {
            for (const auto &dep : h.dependsOn) {
                if (off.count(dep) && off.insert(h.key).second) {
                    grew = true;
                }
            }
        });
    }
    EngineUpdateProposal out = p;
    const auto drop = [&](auto &rows) {
        std::erase_if(rows, [&](const auto &row) { return off.count(row.header.key) > 0; });
    };
    drop(out.playlistsToCreate);
    drop(out.tracksToAdd);
    drop(out.playlistsToRename);
    drop(out.membership);
    drop(out.metadataToEngine);
    drop(out.cuesToEngine);
    drop(out.playlistsToDelete);
    drop(out.tracksToRemove);
    drop(out.restoresToRekordbox);
    drop(out.cuesToRekordbox);
    return out;
}

// `p` with the conflict under `key` answered as the page answers it: the
// chosen side's edits become rows of the conflict's key, ticked, and the
// conflict leaves the list.
EngineUpdateProposal answered(const EngineUpdateProposal &p, const std::string &key, bool rekordboxSide)
{
    EngineUpdateProposal out = p;
    out.conflicts.clear();
    for (const auto &c : p.conflicts) {
        if (c.header.key != key) {
            out.conflicts.push_back(c);
            continue;
        }
        for (EngineUpdateEdit edit : rekordboxSide ? c.rekordboxChoice : c.engineChoice) {
            std::visit(
                [&](auto &e) {
                    using T = std::decay_t<decltype(e)>;
                    // An old item's delete under a reused: key keeps it,
                    // as on the page.
                    if (!parseReusedPlaylistItemKey(e.header.key)) {
                        e.header.key = key;
                    }
                    e.header.checkedByDefault = true;
                    e.header.conflict = false;
                    if constexpr (std::is_same_v<T, PlaylistCreate>) {
                        out.playlistsToCreate.push_back(e);
                    } else if constexpr (std::is_same_v<T, TrackToAdd>) {
                        out.tracksToAdd.push_back(e);
                    } else if constexpr (std::is_same_v<T, PlaylistRename>) {
                        out.playlistsToRename.push_back(e);
                    } else if constexpr (std::is_same_v<T, MembershipEdit>) {
                        out.membership.push_back(e);
                    } else if constexpr (std::is_same_v<T, PlaylistDelete>) {
                        out.playlistsToDelete.push_back(e);
                    } else if constexpr (std::is_same_v<T, TrackToRemove>) {
                        out.tracksToRemove.push_back(e);
                    } else {
                        assert(false && "not a playlist conflict's edit");
                    }
                },
                edit);
        }
    }
    return out;
}

// Every key a record holds in its x lines is of the file's grammar
// (rekordbox_baseline.hpp's itemKey), the grammar Seabass 1.0.1 reads:
// nothing of a plan's own reaches the stick.
bool keysOfTheFileGrammar(const RekordboxBaseline &record)
{
    for (const auto &[key, state] : record.declined) {
        if (!parseItemKey(key)) {
            std::cerr << "FAIL: the record holds the key \"" << key << "\", outside the file's grammar\n";
            return false;
        }
    }
    return true;
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

    // One file spelled in two letter cases (#86, SHAKEDOWN_8): rekordbox
    // lists "Akki (De)", the stick and Engine "AKKI (DE)". matchTracks pairs
    // by the exact path and leaves the two apart, but the planner files
    // every row under its path key, so they still meet: level, not an add
    // and a removal.
    {
        World w;
        w.rekordbox = {rb("1", "Contents/Akki (De)/Midnight/01 Midnight.mp3", {{"P", 0}}, 3)};
        w.engine = {en("e1", "Contents/AKKI (DE)/Midnight/01 Midnight.mp3", {{"P", 0}}, 3)};
        w.rekordboxPlaylists = {{"P", false, 1}};
        w.enginePlaylists = {{"P", false, 1}};
        w.importKey = {{"e1", 14204}};
        assert(plan(w, baselineOf(w)).empty());
        assert(plan(w, std::nullopt).empty());
        std::cout << "one file in two letter cases is level OK\n";
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
        assert(c.rekordboxSide == "Remove this track from Engine" && c.engineSide == "Keep it in Engine");
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

    // #99, as the TESTSHARK round found it: the record holds rekordbox
    // playlist 2 as "T73 Delete Me" with A and B. The DJ deleted it in
    // rekordbox and exported a new, unrelated "T73 New" with C and D, and
    // rekordbox gave the new one id 2. Both lists hold tracks and share
    // none, and Engine's "T73 Delete Me" is as recorded: the old playlist
    // is deleted and the new one created, two items: the create under the
    // id, the delete under a key of the plan's own that no record holds.
    // Saved, the record is rekordbox's and the next analysis offers
    // nothing.
    const auto issueWorld = [](bool engineChanged, int engineCopies) {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {{"T73 Delete Me", 0}}), rb("2", "Music/B.mp3", {{"T73 Delete Me", 1}}),
                       rb("3", "Music/C.mp3"), rb("4", "Music/D.mp3"), rb("5", "Music/E.mp3")};
        w.engine = {en("e1", "Music/A.mp3", {{"T73 Delete Me", 0}}), en("e2", "Music/B.mp3", {{"T73 Delete Me", 1}}),
                    en("e3", "Music/C.mp3"), en("e4", "Music/D.mp3"), en("e5", "Music/E.mp3")};
        w.rekordboxPlaylists = {{"T73 Delete Me", false, 2}};
        w.enginePlaylists = {{"T73 Delete Me", false, engineCopies}};
        const auto base = baselineOf(w);
        if (engineChanged) {
            w.engine[4].playlists = {{"T73 Delete Me", 2}};  // Engine DJ added E since the record
        }
        w.rekordbox = {rb("1", "Music/A.mp3"), rb("2", "Music/B.mp3"), rb("3", "Music/C.mp3", {{"T73 New", 0}}),
                       rb("4", "Music/D.mp3", {{"T73 New", 1}}), rb("5", "Music/E.mp3")};
        w.rekordboxPlaylists = {{"T73 New", false, 2}};
        return std::make_pair(w, base);
    };
    {
        const auto [w, base] = issueWorld(false, 1);
        const auto p = plan(w, base);
        assert(p.playlistsToRename.empty() && "an id rekordbox gave to another playlist is not a rename");
        assert(p.playlistsToDelete.size() == 1);
        const auto &del = p.playlistsToDelete[0];
        assert(del.path == "T73 Delete Me" && del.pdbId == 2 && !del.folder && del.engineMembers == 2);
        assert(del.header.key == "reused:2:T73 Delete Me" && del.header.checkedByDefault && !del.header.conflict);
        assert(!parseItemKey(del.header.key) && "the plan's own key, outside the file's grammar");
        assert(del.header.dependsOn.empty());
        assert(del.header.reason == EngineUpdateReason::RekordboxRemoved);
        assert(del.header.reasonText == "rekordbox deleted this playlist" + Since);
        assert(p.playlistsToCreate.size() == 1);
        const auto &create = p.playlistsToCreate[0];
        assert(create.path == "T73 New" && create.pdbId == 2 && !create.folder);
        assert(create.header.key == "playlist:2" && create.header.checkedByDefault);
        assert(create.header.reason == EngineUpdateReason::RekordboxAdded);
        assert(create.header.dependsOn.empty());
        // The new tracks into the new playlist, each a row under the id;
        // the delete takes the old playlist's tracks with it.
        assert(p.membership.size() == 2);
        std::multiset<std::string> edits;
        for (const auto &m : p.membership) {
            assert(m.header.dependsOn == Deps{"playlist:2"} && m.header.checkedByDefault);
            edits.insert((m.kind == MembershipEdit::Kind::Add ? "+" : "-") + m.playlistPath + ":" + m.pathKey);
        }
        assert((edits == std::multiset<std::string>{"+T73 New:music/c.mp3", "+T73 New:music/d.mp3"}));
        assert(p.conflicts.empty() && p.engineOwnKept.empty());

        World after = w;
        apply(after, p);
        assert(engineOrder(after, "T73 New") == (Deps{"music/c.mp3", "music/d.mp3"}));
        assert(!hasPlaylist(after, "T73 Delete Me"));
        const auto record = recordAfterSave(base, w, p);
        assert(recorded(record, 2) && recorded(record, 2)->path == "T73 New");
        assert(recorded(record, 2)->members == (Deps{"music/c.mp3", "music/d.mp3"}));
        assert(keysOfTheFileGrammar(record));
        assert(plan(after, record).empty() && "the next analysis offers nothing");

        // Every membership row unticked: a new, empty "T73 New", none of
        // A and B in it, and "T73 Delete Me" gone.
        EngineUpdateProposal playlistsOnly = p;
        playlistsOnly.membership.clear();
        World bare = w;
        apply(bare, playlistsOnly);
        assert(engineOrder(bare, "T73 New").empty() && hasPlaylist(bare, "T73 New"));
        assert(!hasPlaylist(bare, "T73 Delete Me"));
        std::cout << "a reused playlist id, no track in common: delete and create, the record right after (#99) OK\n";
    }

    // #99: the pair declined, the delete and the create unticked. Engine
    // keeps "T73 Delete Me"; the record holds nothing under id 2 (the old
    // playlist is gone from rekordbox, and the create was declined) and
    // only the create's decline, so the next analysis lists "T73 Delete
    // Me" as Engine's own and keeps the create out while rekordbox's
    // playlist is as declined. When rekordbox renames it, the create comes
    // back, never a rename of the old playlist, never its delete.
    {
        const auto [w, base] = issueWorld(false, 1);
        const auto p = plan(w, base);
        const std::string oldKey = p.playlistsToDelete.at(0).header.key;
        const auto record = recordAfterSave(base, w, p, {"playlist:2", oldKey});
        assert(!recorded(record, 2) && "neither item of the id recorded");
        assert(record.declined.count("playlist:2") == 1 && record.declined.size() == 1);
        assert(keysOfTheFileGrammar(record));
        const auto again = plan(w, record);
        assert(again.playlistsToRename.empty() && again.playlistsToCreate.empty() && again.playlistsToDelete.empty());
        assert(again.membership.empty() && again.conflicts.empty());
        assert(again.engineOwnKept.size() == 1 && again.engineOwnKept[0].engineOnlyPlaylist
               && again.engineOwnKept[0].playlistPath == "T73 Delete Me" && again.engineOwnKept[0].trackCount == 2);

        World renamed = w;
        renamed.rekordbox[2].playlists = {{"T73 New2", 0}};
        renamed.rekordbox[3].playlists = {{"T73 New2", 1}};
        renamed.rekordboxPlaylists = {{"T73 New2", false, 2}};
        const auto later = plan(renamed, record);
        assert(later.playlistsToRename.empty() && "never a rename of the old playlist");
        assert(later.playlistsToDelete.empty() && "the old playlist is Engine's own now");
        assert(later.playlistsToCreate.size() == 1 && later.playlistsToCreate[0].path == "T73 New2");
        assert(later.playlistsToCreate[0].header.checkedByDefault);
        assert(later.playlistsToCreate[0].header.reason == EngineUpdateReason::RekordboxAdded);
        std::cout << "a declined delete and create: Engine keeps the old playlist as its own, the record neither "
                     "(#99) OK\n";
    }

    // #99, the defect the reviews left open: the pair saved with part of
    // it unticked. The old playlist's item is the plan's own; the record
    // never holds it, so nothing of "T73 Delete Me" lands under the new
    // playlist. Each variant is saved (Engine as the ticked rows leave it,
    // the record as the page makes it), then rekordbox puts A, one of the
    // old playlist's tracks, into "T73 New": the add is offered, ticked,
    // and nothing says Engine took A out.
    // The old playlist has no rows of its own but its delete (its tracks
    // go with it), so the reviews' "its remove rows unticked" is the
    // Membership section unticked:
    //   membership: the whole Membership section unticked;
    //   old: the old playlist's delete unticked, the create ticked;
    //   new: the create unticked, the delete ticked;
    //   both: the delete and the create unticked.
    {
        const auto [w, base] = issueWorld(false, 1);
        const auto p = plan(w, base);
        assert(p.playlistsToDelete.size() == 1 && p.playlistsToCreate.size() == 1);
        const std::string oldKey = p.playlistsToDelete[0].header.key;
        const std::string newKey = p.playlistsToCreate[0].header.key;
        assert(newKey == "playlist:2");
        std::set<std::string> membership;
        for (const auto &m : p.membership) {
            membership.insert(m.header.key);
            assert(m.playlistPath != "T73 Delete Me" && "the old playlist has no rows of its own");
        }
        struct Variant
        {
            const char *what;
            std::set<std::string> unticked;
            bool oldDeleted;
            bool newCreated;
        };
        const std::vector<Variant> variants = {
            {"membership", membership, true, true},
            {"old", {oldKey}, false, true},
            {"new", {newKey}, true, false},
            {"both", {oldKey, newKey}, false, false},
        };
        for (const auto &v : variants) {
            std::cout << "  #99 saved with " << v.what << " unticked\n";
            World after = w;
            apply(after, tickedOnly(p, v.unticked));
            if (hasPlaylist(after, "T73 Delete Me") == v.oldDeleted || hasPlaylist(after, "T73 New") != v.newCreated) {
                std::cerr << "FAIL (" << v.what << "): Engine has \"T73 Delete Me\" "
                          << hasPlaylist(after, "T73 Delete Me") << ", \"T73 New\" " << hasPlaylist(after, "T73 New")
                          << "\n";
            }
            assert(hasPlaylist(after, "T73 Delete Me") != v.oldDeleted);
            assert(hasPlaylist(after, "T73 New") == v.newCreated);
            const auto record = recordAfterSave(base, w, p, v.unticked);
            assert(keysOfTheFileGrammar(record));
            // The record holds rekordbox's playlist 2 or nothing under the
            // id, never the old playlist or its tracks.
            const auto *two = recorded(record, 2);
            assert(!two || two->path == "T73 New");
            if (two) {
                for (const auto &member : two->members) {
                    if (member == "music/a.mp3" || member == "music/b.mp3") {
                        std::cerr << "FAIL (" << v.what << "): the record holds " << member << " under \"T73 New\"\n";
                    }
                    assert(member != "music/a.mp3" && member != "music/b.mp3");
                }
            }
            assert(!two == !v.newCreated && "the create unticked: no record of the id");

            // The next analysis, nothing changed since: nothing of the
            // old playlist offered again, no conflict, no rename; left on
            // Engine, it is Engine's own playlist.
            const auto next = plan(after, record);
            assert(next.conflicts.empty() && next.playlistsToRename.empty() && next.playlistsToDelete.empty());
            assert(next.playlistsToCreate.empty() && "a declined create stays out");
            const bool oldListed = std::any_of(next.engineOwnKept.begin(), next.engineOwnKept.end(), [](const auto &k) {
                return k.engineOnlyPlaylist && k.playlistPath == "T73 Delete Me";
            });
            assert(oldListed == !v.oldDeleted && "left on Engine, the old playlist is Engine's own");

            // rekordbox puts A into "T73 New" (when Engine has it).
            if (!v.newCreated) {
                continue;
            }
            World added = after;
            added.rekordbox[0].playlists = {{"T73 New", 2}};
            const auto withA = plan(added, record);
            bool offered = false;
            for (const auto &m : withA.membership) {
                if (m.kind == MembershipEdit::Kind::Add && m.pathKey == "music/a.mp3" && m.playlistPath == "T73 New") {
                    offered = m.header.checkedByDefault && m.header.reason == EngineUpdateReason::RekordboxAdded;
                }
            }
            for (const auto &k : withA.engineOwnKept) {
                if (k.header.key == "member:2:music/a.mp3") {
                    std::cerr << "FAIL (" << v.what << "): \"" << k.header.reasonText << "\"\n";
                }
                assert(k.header.key != "member:2:music/a.mp3" && "nothing says Engine took A out");
            }
            assert(offered && "the add of A is offered, ticked");
        }
        std::cout << "a reused id saved with parts unticked: the old playlist never in the record, a later add "
                     "offered (#99) OK\n";
    }

    // #99, declines recorded for the old playlist's id. A declined entry
    // matches on its key and rekordbox's state, and neither names the
    // playlist, so the new item under the id must not inherit them.
    //   a: the record declines the add of z into Warmup (rekordbox's
    //      Warmup held z and x, Engine's x). rekordbox then deletes Warmup
    //      and gives id 5 to Peak, z and w. The record alone shares nothing
    //      with Peak, but rekordbox's Warmup did hold z: not certain, so
    //      the id pairs as before this rule, a rename, the decline the
    //      same playlist's (a rename keeps it). Without the decline, the
    //      rule applies.
    //   b: the record declines a rename of Warmup to Peak, and the order
    //      of 5. rekordbox's Peak now holds only w: delete and create, the
    //      create offered and ticked (the old decline matches its state,
    //      "list:Peak", but is another item's), the order not suppressed,
    //      and the record after the save holds no decline under 5.
    {
        const auto world = [](bool declinedZ) {
            World w;
            w.rekordbox = {rb("1", "Music/X.mp3", {{"Warmup", 0}}), rb("2", "Music/Z.mp3"), rb("3", "Music/W.mp3")};
            w.engine = {en("e1", "Music/X.mp3", {{"Warmup", 0}}), en("e2", "Music/Z.mp3"), en("e3", "Music/W.mp3")};
            w.rekordboxPlaylists = {{"Warmup", false, 5}};
            w.enginePlaylists = {{"Warmup", false, 1}};
            auto base = baselineOf(w);
            if (declinedZ) {
                base.declined["member:5:music/z.mp3"] = engineUpdateStateHash("member:in:after:");
            }
            w.rekordbox[0].playlists.clear();
            w.rekordbox[1].playlists = {{"Peak", 0}};
            w.rekordbox[2].playlists = {{"Peak", 1}};
            w.rekordboxPlaylists = {{"Peak", false, 5}};
            return std::make_pair(w, base);
        };
        {
            const auto [w, base] = world(true);
            const auto p = plan(w, base);
            if (!p.playlistsToCreate.empty()) {
                std::cerr << "FAIL: a playlist rekordbox held z in is taken as unrelated to one holding z\n";
            }
            assert(p.playlistsToRename.size() == 1 && p.playlistsToRename[0].fromPath == "Warmup"
                   && p.playlistsToRename[0].toPath == "Peak");
            assert(p.playlistsToCreate.empty() && p.playlistsToDelete.empty());
            assert(std::count(p.declinedSuppressed.begin(), p.declinedSuppressed.end(), "member:5:music/z.mp3") == 1
                   && "the same playlist renamed keeps its decline, as before this rule");
        }
        {
            const auto [w, base] = world(false);
            const auto p = plan(w, base);
            assert(p.playlistsToDelete.size() == 1 && p.playlistsToCreate.size() == 1 && p.playlistsToRename.empty());
        }
        {
            World w;
            w.rekordbox = {rb("1", "Music/X.mp3", {{"Warmup", 0}}), rb("2", "Music/W.mp3")};
            w.engine = {en("e1", "Music/X.mp3", {{"Warmup", 0}}), en("e2", "Music/W.mp3")};
            w.rekordboxPlaylists = {{"Warmup", false, 5}};
            w.enginePlaylists = {{"Warmup", false, 1}};
            auto base = baselineOf(w);
            base.declined["playlist:5"] = engineUpdateStateHash("list:Peak");
            base.declined["member:5:music/w.mp3"] = engineUpdateStateHash("member:in:after:");
            base.declined["order:5"] = "an order declined for Warmup";
            base.declined["rating:music/x.mp3"] = "kept";
            w.rekordbox[0].playlists.clear();
            w.rekordbox[1].playlists = {{"Peak", 0}};
            w.rekordboxPlaylists = {{"Peak", false, 5}};
            // A declined member key the new list holds: rekordbox's Warmup
            // held w. Not certain, as in a: the id pairs as a rename, the
            // very rename the record declines, so it stays out.
            {
                const auto uncertain = plan(w, base);
                assert(uncertain.playlistsToCreate.empty() && uncertain.playlistsToDelete.empty());
                assert(uncertain.playlistsToRename.empty());
                assert(std::count(uncertain.declinedSuppressed.begin(), uncertain.declinedSuppressed.end(),
                                  "playlist:5")
                       == 1);
            }
            base.declined.erase("member:5:music/w.mp3");
            const auto p = plan(w, base);
            assert(p.playlistsToRename.empty() && p.playlistsToDelete.size() == 1);
            if (p.playlistsToCreate.size() != 1) {
                std::cerr << "FAIL: the create of Peak is suppressed by a decline recorded for Warmup\n";
            }
            assert(p.playlistsToCreate.size() == 1 && p.playlistsToCreate[0].path == "Peak");
            assert(p.playlistsToCreate[0].header.checkedByDefault);
            for (const auto &key : p.declinedSuppressed) {
                assert(key.rfind("playlist:5", 0) != 0 && key.rfind("member:5:", 0) != 0 && key.rfind("order:5", 0) != 0);
            }
            World after = w;
            apply(after, p);
            const auto record = recordAfterSave(base, w, p);
            assert(keysOfTheFileGrammar(record));
            for (const auto &[key, state] : record.declined) {
                if (key.rfind("playlist:5", 0) == 0 || key.rfind("member:5:", 0) == 0 || key.rfind("order:5", 0) == 0) {
                    std::cerr << "FAIL: the record carries the stale decline " << key << "\n";
                }
                assert(key.rfind("playlist:5", 0) != 0 && key.rfind("member:5:", 0) != 0 && key.rfind("order:5", 0) != 0);
            }
            assert(plan(after, record).empty());
        }
        std::cout << "declines recorded for a reused id: a rename where rekordbox's list held the track, never "
                     "inherited by the new item (#99) OK\n";
    }

    // #99, after the old playlist was left on Engine (its delete unticked):
    // rekordbox later makes a playlist at the old path again, id 9, with A
    // and E. Engine's leftover is paired with it by path (the planner's
    // rule for a playlist with no record), so A is level, E (rekordbox's)
    // is added, ticked, and B, which only Engine's copy holds and no
    // record says which side changed, is a conflict. Nothing written over
    // without asking.
    {
        const auto [w, base] = issueWorld(false, 1);
        const auto p = plan(w, base);
        const std::string oldKey = p.playlistsToDelete.at(0).header.key;
        World after = w;
        apply(after, tickedOnly(p, {oldKey}));
        const auto record = recordAfterSave(base, w, p, {oldKey});
        assert(hasPlaylist(after, "T73 Delete Me") && engineOrder(after, "T73 Delete Me").size() == 2);
        World again = after;
        again.rekordbox[0].playlists = {{"T73 Delete Me", 0}};
        again.rekordbox[4].playlists = {{"T73 Delete Me", 1}};
        again.rekordboxPlaylists.push_back({"T73 Delete Me", false, 9});
        const auto q = plan(again, record);
        for (const auto &m : q.membership) {
            std::cout << "  old path again: " << (m.kind == MembershipEdit::Kind::Add ? "add " : "remove ")
                      << m.playlistPath << ":" << m.pathKey << " " << (m.header.checkedByDefault ? "ticked" : "unticked")
                      << "\n";
        }
        for (const auto &c : q.conflicts) {
            std::cout << "  old path again: conflict " << c.header.key << ": " << c.header.reasonText << "\n";
        }
        assert(q.playlistsToCreate.empty() && q.playlistsToRename.empty() && q.playlistsToDelete.empty());
        assert(q.membership.size() == 1 && q.membership[0].kind == MembershipEdit::Kind::Add);
        assert(q.membership[0].playlistPath == "T73 Delete Me" && q.membership[0].pathKey == "music/e.mp3");
        assert(q.membership[0].header.checkedByDefault);
        assert(q.conflicts.size() == 1 && q.conflicts[0].header.key == "member:9:music/b.mp3");
        assert(q.conflicts[0].header.reason == EngineUpdateReason::NoBaselineEngineMember);
        assert(q.engineOwnKept.empty() && "Engine's leftover is rekordbox's playlist again, not Engine's own");
        std::cout << "a playlist at the old path again takes Engine's leftover: rekordbox's tracks added, Engine's "
                     "own a conflict (#99) OK\n";
    }

    // #99, the known reading: a real rename in rekordbox ("Warmup" to
    // "Peak", id 5 kept) that also replaced every track cannot be told
    // from a deleted playlist whose id went to a new one. It is read as
    // Warmup deleted and Peak created: Engine's playlist loses its
    // identity and its place among its siblings. Accepted, pinned.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {{"Warmup", 0}}), rb("2", "Music/B.mp3", {{"Warmup", 1}}),
                       rb("3", "Music/C.mp3"), rb("4", "Music/D.mp3")};
        w.engine = {en("e1", "Music/A.mp3", {{"Warmup", 0}}), en("e2", "Music/B.mp3", {{"Warmup", 1}}),
                    en("e3", "Music/C.mp3"), en("e4", "Music/D.mp3")};
        w.rekordboxPlaylists = {{"Warmup", false, 5}};
        w.enginePlaylists = {{"Warmup", false, 1}};
        const auto base = baselineOf(w);
        w.rekordbox[0].playlists.clear();
        w.rekordbox[1].playlists.clear();
        w.rekordbox[2].playlists = {{"Peak", 0}};
        w.rekordbox[3].playlists = {{"Peak", 1}};
        w.rekordboxPlaylists = {{"Peak", false, 5}};
        const auto p = plan(w, base);
        assert(p.playlistsToRename.empty());
        assert(p.playlistsToDelete.size() == 1 && p.playlistsToDelete[0].path == "Warmup");
        assert(p.playlistsToCreate.size() == 1 && p.playlistsToCreate[0].path == "Peak");
        World after = w;
        apply(after, p);
        assert(!hasPlaylist(after, "Warmup") && engineOrder(after, "Peak") == (Deps{"music/c.mp3", "music/d.mp3"}));
        assert(plan(after, recordAfterSave(base, w, p)).empty());
        std::cout << "a rename that also replaced every track reads as delete and create, the accepted reading (#99) "
                     "OK\n";
    }

    // #99: Engine's "T73 Delete Me" changed since the record, so its delete
    // would be a conflict: the id pairs them as before this rule, a
    // rename; saved, the record and Engine agree and nothing is left to
    // offer but Engine's own track.
    {
        const auto [w, base] = issueWorld(true, 1);
        const auto p = plan(w, base);
        assert(p.playlistsToRename.size() == 1 && p.playlistsToRename[0].fromPath == "T73 Delete Me"
               && p.playlistsToRename[0].toPath == "T73 New");
        assert(p.playlistsToDelete.empty() && p.playlistsToCreate.empty() && p.conflicts.empty());
        World after = w;
        apply(after, p);
        const auto record = recordAfterSave(base, w, p);
        const auto next = plan(after, record);
        assert(next.empty() && next.conflicts.empty());
        std::cout << "a reused id whose old playlist Engine changed: as before, a rename (#99) OK\n";
    }

    // #99: the old path is two playlists in Engine: as before, a refusal
    // under the id and nothing created; the record keeps the old playlist.
    {
        const auto [w, base] = issueWorld(false, 2);
        const auto p = plan(w, base);
        assert(p.playlistsToCreate.empty() && p.playlistsToDelete.empty() && p.playlistsToRename.empty());
        assert(p.conflicts.size() == 1 && p.conflicts[0].header.key == "playlist:2");
        assert(p.conflicts[0].header.reason == EngineUpdateReason::EnginePathAmbiguous);
        const auto record = recordAfterSave(base, w, p);
        assert(recorded(record, 2) && recorded(record, 2)->path == "T73 Delete Me");
        std::cout << "a reused id at an ambiguous old path: as before, refused and kept (#99) OK\n";
    }

    // #99: folder F renamed to G, its playlist X empty in the record and in
    // rekordbox: X moves with its folder and needs nothing.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3")};
        w.engine = {en("e1", "Music/A.mp3")};
        w.rekordboxPlaylists = {{"F", true, 3}, {"F/X", false, 4}};
        w.enginePlaylists = {{"F", true, 1}, {"F/X", false, 1}};
        const auto base = baselineOf(w);
        w.rekordboxPlaylists = {{"G", true, 3}, {"G/X", false, 4}};
        const auto p = plan(w, base);
        assert(p.playlistsToRename.size() == 1 && p.playlistsToRename[0].fromPath == "F"
               && p.playlistsToRename[0].toPath == "G");
        assert(p.playlistsToCreate.empty() && p.playlistsToDelete.empty() && p.membership.empty());
        World after = w;
        apply(after, p);
        assert(hasPlaylist(after, "G/X") && !hasPlaylist(after, "F/X"));
        assert(plan(after, recordAfterSave(base, w, p)).empty());
        std::cout << "a folder renamed with an empty playlist in it: only the folder's rename (#99) OK\n";
    }

    // #99: a recorded list and rekordbox's under its id that share tracks,
    // or one with no tracks: as before this rule, a rename by id. The
    // last is the House/Warmup case: a small new list borrowing from a
    // big deleted one still reads as its rename, the reviewer's case, left
    // as it was.
    {
        struct Case
        {
            const char *what;
            std::vector<std::string> before;
            std::vector<std::string> now;
        };
        const std::vector<Case> cases = {
            {"one of three in common", {"A", "B", "C", "D", "E"}, {"A", "F", "G"}},
            {"two of three in common", {"A", "B", "C", "D"}, {"A", "B", "E"}},
            {"the recorded list empty", {}, {"A", "B"}},
            {"rekordbox's list empty", {"A", "B"}, {}},
            {"House and Warmup", {"A", "B", "C", "D", "E", "F", "G"}, {"A", "B", "C", "H", "I", "J"}},
        };
        for (const auto &c : cases) {
            World w;
            int id = 0;
            for (const char *name : {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J"}) {
                const std::string relative = std::string("Music/") + name + ".mp3";
                const std::string number = std::to_string(++id);
                w.rekordbox.push_back(rb(number, relative));
                w.engine.push_back(en("e" + number, relative));
            }
            const auto place = [](std::vector<Track> &tracks, const std::string &list,
                                  const std::vector<std::string> &names) {
                for (std::size_t i = 0; i < names.size(); ++i) {
                    byKey(tracks, "music/" + lowerKey(names[i]) + ".mp3")
                        ->playlists.push_back({list, static_cast<int>(i)});
                }
            };
            place(w.rekordbox, "Before", c.before);
            place(w.engine, "Before", c.before);
            w.rekordboxPlaylists = {{"Before", false, 9}};
            w.enginePlaylists = {{"Before", false, 1}};
            const auto base = baselineOf(w);
            for (auto &t : w.rekordbox) {
                t.playlists.clear();
            }
            place(w.rekordbox, "After", c.now);
            w.rekordboxPlaylists = {{"After", false, 9}};
            const auto p = plan(w, base);
            if (p.playlistsToRename.size() != 1) {
                std::cerr << "FAIL (" << c.what << "): not planned as a rename\n";
            }
            assert(p.playlistsToRename.size() == 1 && p.playlistsToRename[0].fromPath == "Before"
                   && p.playlistsToRename[0].toPath == "After");
            assert(p.playlistsToCreate.empty() && p.playlistsToDelete.empty() && p.conflicts.empty());
            World after = w;
            apply(after, p);
            std::vector<std::string> want;
            for (const auto &name : c.now) {
                want.push_back("music/" + lowerKey(name) + ".mp3");
            }
            assert(engineOrder(after, "After") == want);
            assert(plan(after, recordAfterSave(base, w, p)).empty());
        }
        std::cout << "a reused id with tracks in common, or an empty list: as before, a rename (#99) OK\n";
    }

    // #99: an id rekordbox gave to an item of the other kind. A playlist's
    // id to a folder (with a playlist of its own inside), and a folder's to
    // a playlist: never a rename; the old item deleted, the new created.
    // A folder that holds something of Engine's cannot be deleted plainly:
    // as before, a rename.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {{"Old", 0}}), rb("2", "Music/B.mp3", {{"Old", 1}}),
                       rb("3", "Music/C.mp3")};
        w.engine = {en("e1", "Music/A.mp3", {{"Old", 0}}), en("e2", "Music/B.mp3", {{"Old", 1}}),
                    en("e3", "Music/C.mp3")};
        w.rekordboxPlaylists = {{"Old", false, 5}};
        w.enginePlaylists = {{"Old", false, 1}};
        const auto base = baselineOf(w);
        w.rekordbox[0].playlists.clear();
        w.rekordbox[1].playlists.clear();
        w.rekordbox[2].playlists = {{"NewF/Kid", 0}};
        w.rekordboxPlaylists = {{"NewF", true, 5}, {"NewF/Kid", false, 6}};
        const auto p = plan(w, base);
        if (!p.playlistsToRename.empty()) {
            std::cerr << "FAIL: a playlist's id given to a folder renames \"" << p.playlistsToRename[0].fromPath
                      << "\" to \"" << p.playlistsToRename[0].toPath << "\"\n";
        }
        assert(p.playlistsToRename.empty());
        assert(p.playlistsToDelete.size() == 1 && p.playlistsToDelete[0].path == "Old");
        assert(p.playlistsToCreate.size() == 2 && p.playlistsToCreate[0].path == "NewF" && p.playlistsToCreate[0].folder);
        assert(p.playlistsToCreate[1].path == "NewF/Kid" && p.playlistsToCreate[1].header.dependsOn == Deps{"playlist:5"});
        World after = w;
        apply(after, p);
        assert(!hasPlaylist(after, "Old") && engineOrder(after, "NewF/Kid") == Deps{"music/c.mp3"});
        assert(plan(after, recordAfterSave(base, w, p)).empty());
        // Both declined: the record holds neither item of id 5.
        assert(p.playlistsToDelete[0].header.key == "reused:5:Old");
        const auto declined = recordAfterSave(base, w, p, {"playlist:5", "reused:5:Old"});
        assert(!recorded(declined, 5) && keysOfTheFileGrammar(declined));

        for (const bool engineHoldsSomething : {false, true}) {
            World v;
            v.rekordbox = {rb("1", "Music/A.mp3")};
            v.engine = {en("e1", "Music/A.mp3")};
            v.rekordboxPlaylists = {{"OldF", true, 7}};
            v.enginePlaylists = {{"OldF", true, 1}};
            const auto vbase = baselineOf(v);
            if (engineHoldsSomething) {
                v.enginePlaylists.push_back({"OldF/Own", false, 1});
            }
            v.rekordbox[0].playlists = {{"NewP", 0}};
            v.rekordboxPlaylists = {{"NewP", false, 7}};
            const auto q = plan(v, vbase);
            if (engineHoldsSomething) {
                // #124: never a rename; the refusal of the next case.
                assert(q.playlistsToRename.empty());
                assert(q.playlistsToDelete.empty() && q.playlistsToCreate.empty());
                assert(q.conflicts.size() == 1 && q.conflicts[0].header.key == "playlist:7");
                continue;
            }
            assert(q.playlistsToRename.empty());
            assert(q.playlistsToDelete.size() == 1 && q.playlistsToDelete[0].path == "OldF" && q.playlistsToDelete[0].folder);
            assert(q.playlistsToCreate.size() == 1 && q.playlistsToCreate[0].path == "NewP" && !q.playlistsToCreate[0].folder);
            World vafter = v;
            apply(vafter, q);
            assert(!hasPlaylist(vafter, "OldF") && engineOrder(vafter, "NewP") == Deps{"music/a.mp3"});
            assert(plan(vafter, recordAfterSave(vbase, v, q)).empty());
        }
        std::cout << "an id reused across playlist and folder: delete and create, as before where unsafe (#99) OK\n";
    }

    // #124: an id whose kind changed between the record and the export is
    // never a rename.
    //   a: the record's empty folder "OldF" (id 7), which Engine's reader
    //      gives as a playlist holding nothing; rekordbox gives 7 to the
    //      new playlist "NewP": OldF deleted, NewP created.
    //   b: the reverse, the record's playlist "OldP" (id 8, A) given to
    //      the new folder "NewF", with Engine's OldP holding B too since
    //      the record: Engine's copy cannot be deleted plainly, so one
    //      refusal under the id that says why, nothing written, and
    //      Engine's OldP accounted for by it.
    //   c: the record's folder "OldF", which Engine no longer has: only
    //      the create, no conflict that calls it a rename.
    //   d: the record's folder "OldF" with Engine's own playlist inside:
    //      the refusal, as in b.
    {
        const auto folderToPlaylist = [](bool engineHasIt, bool engineHoldsSomething) {
            World v;
            v.rekordbox = {rb("1", "Music/A.mp3")};
            v.engine = {en("e1", "Music/A.mp3")};
            v.rekordboxPlaylists = {{"OldF", true, 7}};
            const auto vbase = baselineOf(v);
            if (engineHasIt) {
                v.enginePlaylists = {{"OldF", false, 1}};  // an empty folder, as Engine's reader gives it
            }
            if (engineHoldsSomething) {
                v.enginePlaylists = {{"OldF", true, 1}, {"OldF/Own", false, 1}};
            }
            v.rekordbox[0].playlists = {{"NewP", 0}};
            v.rekordboxPlaylists = {{"NewP", false, 7}};
            return std::make_pair(v, vbase);
        };
        {
            const auto [v, vbase] = folderToPlaylist(true, false);
            const auto q = plan(v, vbase);
            if (!q.playlistsToRename.empty()) {
                std::cerr << "FAIL (a): the folder's id given to a playlist renames \"" << q.playlistsToRename[0].fromPath
                          << "\" to \"" << q.playlistsToRename[0].toPath << "\"\n";
            }
            assert(q.playlistsToRename.empty());
            assert(q.playlistsToDelete.size() == 1 && q.playlistsToDelete[0].path == "OldF");
            assert(q.playlistsToDelete[0].header.key == "reused:7:OldF" && q.playlistsToDelete[0].header.checkedByDefault);
            assert(q.playlistsToCreate.size() == 1 && q.playlistsToCreate[0].path == "NewP");
            assert(q.playlistsToCreate[0].header.key == "playlist:7" && !q.playlistsToCreate[0].folder);
            assert(q.conflicts.empty());
            World after = v;
            apply(after, q);
            assert(!hasPlaylist(after, "OldF") && engineOrder(after, "NewP") == Deps{"music/a.mp3"});
            const auto record = recordAfterSave(vbase, v, q);
            assert(keysOfTheFileGrammar(record));
            assert(plan(after, record).empty());
        }
        {
            World v;
            v.rekordbox = {rb("1", "Music/A.mp3", {{"OldP", 0}}), rb("2", "Music/B.mp3")};
            v.engine = {en("e1", "Music/A.mp3", {{"OldP", 0}}), en("e2", "Music/B.mp3")};
            v.rekordboxPlaylists = {{"OldP", false, 8}};
            v.enginePlaylists = {{"OldP", false, 1}};
            const auto vbase = baselineOf(v);
            v.engine[1].playlists = {{"OldP", 1}};  // Engine DJ added B since the record
            v.rekordbox[0].playlists.clear();
            v.rekordboxPlaylists = {{"NewF", true, 8}};
            const auto q = plan(v, vbase);
            if (!q.playlistsToRename.empty()) {
                std::cerr << "FAIL (b): the playlist's id given to a folder renames \"" << q.playlistsToRename[0].fromPath
                          << "\" to \"" << q.playlistsToRename[0].toPath << "\"\n";
            }
            assert(q.playlistsToRename.empty());
            assert(q.playlistsToDelete.empty() && q.playlistsToCreate.empty() && q.membership.empty());
            assert(q.conflicts.size() == 1);
            const auto &c = q.conflicts[0];
            std::cout << "  #124 b: " << c.header.reasonText << "\n";
            assert(c.header.key == "playlist:8" && c.header.conflict && !c.header.checkedByDefault);
            assert(c.rekordboxChoice.empty() && c.engineChoice.empty() && "a refusal");
            assert(c.header.reason == EngineUpdateReason::BothChanged);
            assert(c.header.reasonText
                   == "rekordbox gave the id of the playlist \"OldP\" to the new folder \"NewF\", so it is no rename; "
                      "Engine's \"OldP\" is not as Seabass last recorded it, so Seabass does not delete it: delete "
                      "or rename it in Engine DJ, then sync again");
            assert(q.engineOwnKept.empty() && "Engine's OldP is accounted for by the refusal");
            // The record keeps the old playlist under the id: the refusal
            // comes back until Engine's copy is out of the way.
            const auto record = recordAfterSave(vbase, v, q);
            assert(recorded(record, 8) && recorded(record, 8)->path == "OldP");
            assert(keysOfTheFileGrammar(record));
            // Kept whole, its tracks with it, so the next analysis refuses
            // for the same reason.
            if (recorded(record, 8)->folder || recorded(record, 8)->members != Deps{"music/a.mp3"}) {
                std::cerr << "FAIL (b): the record keeps \"OldP\" with " << recorded(record, 8)->members.size()
                          << " member(s)\n";
            }
            assert(!recorded(record, 8)->folder && recorded(record, 8)->members == Deps{"music/a.mp3"});
            const auto again = plan(v, record);
            assert(again.conflicts.size() == 1 && again.conflicts[0].header.reasonText == c.header.reasonText);
            // Renamed in Engine DJ: nothing at the old path, so the folder
            // is created and Engine's renamed copy is Engine's own.
            World renamed = v;
            renamed.enginePlaylists = {{"Kept", false, 1}};
            for (auto &t : renamed.engine) {
                for (auto &m : t.playlists) {
                    m.name = "Kept";
                }
            }
            const auto later = plan(renamed, record);
            assert(later.conflicts.empty() && later.playlistsToRename.empty() && later.playlistsToDelete.empty());
            assert(later.playlistsToCreate.size() == 1 && later.playlistsToCreate[0].path == "NewF"
                   && later.playlistsToCreate[0].folder);
        }
        {
            const auto [v, vbase] = folderToPlaylist(false, false);
            const auto q = plan(v, vbase);
            for (const auto &c : q.conflicts) {
                std::cerr << "FAIL (c): conflict \"" << c.header.reasonText << "\"\n";
            }
            assert(q.playlistsToRename.empty() && q.conflicts.empty() && q.playlistsToDelete.empty());
            assert(q.playlistsToCreate.size() == 1 && q.playlistsToCreate[0].path == "NewP");
            assert(q.playlistsToCreate[0].header.reason == EngineUpdateReason::RekordboxAdded);
            assert(q.membership.size() == 1 && q.membership[0].pathKey == "music/a.mp3");
            World after = v;
            apply(after, q);
            assert(plan(after, recordAfterSave(vbase, v, q)).empty());
        }
        {
            const auto [v, vbase] = folderToPlaylist(true, true);
            const auto q = plan(v, vbase);
            assert(q.playlistsToRename.empty() && q.playlistsToDelete.empty() && q.playlistsToCreate.empty());
            assert(q.conflicts.size() == 1 && q.conflicts[0].header.key == "playlist:7");
            assert(q.conflicts[0].rekordboxChoice.empty() && q.conflicts[0].engineChoice.empty());
            std::cout << "  #124 d: " << q.conflicts[0].header.reasonText << "\n";
            assert(q.conflicts[0].header.reasonText
                   == "rekordbox gave the id of the folder \"OldF\" to the new playlist \"NewP\", so it is no rename; "
                      "Engine's \"OldF\" is not as Seabass last recorded it, so Seabass does not delete it: delete "
                      "or rename it in Engine DJ, then sync again");
            // Engine's own playlist inside is still listed as Engine's own.
            assert(q.engineOwnKept.size() == 1 && q.engineOwnKept[0].playlistPath == "OldF/Own");
            // Saved with the refusal open: the record keeps the folder
            // under the id, and the next analysis refuses again.
            const auto record = recordAfterSave(vbase, v, q);
            assert(keysOfTheFileGrammar(record));
            assert(recorded(record, 7) && recorded(record, 7)->path == "OldF" && recorded(record, 7)->folder);
            const auto again = plan(v, record);
            assert(again.playlistsToRename.empty() && again.playlistsToCreate.empty());
            assert(again.conflicts.size() == 1 && again.conflicts[0].header.reasonText == q.conflicts[0].header.reasonText);
        }
        // e: the recorded playlist "OldP" (id 8, A), as Engine has it, given
        //    to the new folder "P/NewF" inside the new folder "P": the
        //    create waits for P (a refusal that says so), P is created.
        //    Saved, the record keeps OldP with A, so the next analysis
        //    finds the pair plain: OldP deleted, P/NewF created.
        {
            World v;
            v.rekordbox = {rb("1", "Music/A.mp3", {{"OldP", 0}})};
            v.engine = {en("e1", "Music/A.mp3", {{"OldP", 0}})};
            v.rekordboxPlaylists = {{"OldP", false, 8}};
            v.enginePlaylists = {{"OldP", false, 1}};
            const auto vbase = baselineOf(v);
            v.rekordbox[0].playlists.clear();
            v.rekordboxPlaylists = {{"P", true, 20}, {"P/NewF", true, 8}};
            const auto q = plan(v, vbase);
            assert(q.playlistsToRename.empty() && q.playlistsToDelete.empty());
            assert(q.playlistsToCreate.size() == 1 && q.playlistsToCreate[0].path == "P");
            assert(q.conflicts.size() == 1 && q.conflicts[0].header.key == "playlist:8");
            std::cout << "  #124 e: " << q.conflicts[0].header.reasonText << "\n";
            assert(q.conflicts[0].header.reasonText
                   == "rekordbox gave the id of the playlist \"OldP\" to the new folder \"P/NewF\", so it is no "
                      "rename; its folder \"P\" is not in Engine yet, so Seabass does not replace the one with the "
                      "other in this save: save the rest, then sync again");
            assert(q.conflicts[0].header.dependsOn == Deps{"playlist:20"} && "the new parent's create, kept");
            World after = v;
            apply(after, q);
            const auto record = recordAfterSave(vbase, v, q);
            assert(recorded(record, 8) && recorded(record, 8)->members == Deps{"music/a.mp3"});
            const auto next = plan(after, record);
            for (const auto &c : next.conflicts) {
                std::cerr << "FAIL (e): after the save, \"" << c.header.reasonText << "\"\n";
            }
            assert(next.conflicts.empty());
            assert(next.playlistsToDelete.size() == 1 && next.playlistsToDelete[0].header.key == "reused:8:OldP");
            assert(next.playlistsToCreate.size() == 1 && next.playlistsToCreate[0].path == "P/NewF");
        }
        // f: as b, the new folder holding a playlist of its own, "NewF/C"
        //    with B, the folder inside the new folder "P". Nothing inside
        //    the refused folder is planned to be written: C is a refusal
        //    of its own, so no save makes NewF through it. Each refusal
        //    keeps its parents' rows as dependencies.
        {
            World v;
            v.rekordbox = {rb("1", "Music/A.mp3", {{"OldP", 0}}), rb("2", "Music/B.mp3")};
            v.engine = {en("e1", "Music/A.mp3", {{"OldP", 0}}), en("e2", "Music/B.mp3")};
            v.rekordboxPlaylists = {{"OldP", false, 8}};
            v.enginePlaylists = {{"OldP", false, 1}};
            const auto vbase = baselineOf(v);
            v.engine[1].playlists = {{"OldP", 1}};  // Engine DJ added B since the record
            v.rekordbox[0].playlists.clear();
            v.rekordbox[1].playlists = {{"P/NewF/C", 0}};
            v.rekordboxPlaylists = {{"P", true, 20}, {"P/NewF", true, 8}, {"P/NewF/C", false, 9}};
            const auto q = plan(v, vbase);
            for (const auto &c : q.playlistsToCreate) {
                if (c.path != "P") {
                    std::cerr << "FAIL (f): \"" << c.path << "\" is created inside the refused folder\n";
                }
            }
            assert(q.playlistsToCreate.size() == 1 && q.playlistsToCreate[0].path == "P");
            assert(q.membership.empty() && q.playlistsToRename.empty() && q.playlistsToDelete.empty());
            assert(q.conflicts.size() == 2);
            for (const auto &c : q.conflicts) {
                assert(c.rekordboxChoice.empty() && c.engineChoice.empty() && "refusals");
                assert(std::count(c.header.dependsOn.begin(), c.header.dependsOn.end(), "playlist:20") == 1);
            }
            assert(q.conflicts[0].header.key == "playlist:8" && q.conflicts[1].header.key == "playlist:9");
            std::cout << "  #124 f: " << q.conflicts[1].header.reasonText << "\n";
            assert(q.conflicts[1].header.reasonText
                   == "Seabass does not make its folder \"P/NewF\" in this save (see that row), so not this "
                      "playlist either");
            // Saved: the record knows nothing of C, so it is offered as
            // new once its folder is made.
            const auto record = recordAfterSave(vbase, v, q);
            assert(!recorded(record, 9));
        }
        // g: the recorded folder "OldF" (id 7) still holds its recorded
        //    playlist "OldF/X" (id 11, A), which rekordbox deleted; the
        //    folder's id went to the new playlist "NewP". X's delete is
        //    offered; the refusal says a save with it ticked clears the
        //    way, and after that save the pair is plain.
        {
            World v;
            v.rekordbox = {rb("1", "Music/A.mp3", {{"OldF/X", 0}}), rb("2", "Music/B.mp3")};
            v.engine = {en("e1", "Music/A.mp3", {{"OldF/X", 0}}), en("e2", "Music/B.mp3")};
            v.rekordboxPlaylists = {{"OldF", true, 7}, {"OldF/X", false, 11}};
            v.enginePlaylists = {{"OldF", true, 1}, {"OldF/X", false, 1}};
            const auto vbase = baselineOf(v);
            v.rekordbox[0].playlists.clear();
            v.rekordbox[1].playlists = {{"NewP", 0}};
            v.rekordboxPlaylists = {{"NewP", false, 7}};
            const auto q = plan(v, vbase);
            assert(q.playlistsToRename.empty() && q.playlistsToCreate.empty());
            assert(q.playlistsToDelete.size() == 1 && q.playlistsToDelete[0].path == "OldF/X");
            assert(q.playlistsToDelete[0].header.key == "playlist:11" && q.playlistsToDelete[0].header.checkedByDefault);
            assert(q.conflicts.size() == 1 && q.conflicts[0].header.key == "playlist:7");
            std::cout << "  #124 g: " << q.conflicts[0].header.reasonText << "\n";
            assert(q.conflicts[0].header.reasonText
                   == "rekordbox gave the id of the folder \"OldF\" to the new playlist \"NewP\", so it is no "
                      "rename; Engine's \"OldF\" still holds what rekordbox moved out of it or deleted from it, "
                      "offered here, so Seabass does not delete the folder yet: save with those ticked, then sync "
                      "again");
            World after = v;
            apply(after, q);
            const auto record = recordAfterSave(vbase, v, q);
            const auto next = plan(after, record);
            assert(next.conflicts.empty() && next.playlistsToRename.empty());
            assert(next.playlistsToDelete.size() == 1 && next.playlistsToDelete[0].header.key == "reused:7:OldF");
            assert(next.playlistsToCreate.size() == 1 && next.playlistsToCreate[0].path == "NewP");
        }
        // h: as b, and the recorded playlist "P" (id 9, X and Y, as Engine
        //    has it) moved by rekordbox into the refused folder "NewF",
        //    where rekordbox also took Y out and put Z in. P's move is a
        //    refusal of its own; while it is open the record keeps P whole
        //    (X and Y) and Engine's P is accounted for by it, not listed as
        //    Engine's own. Once Engine's OldP is out of the way, P moves
        //    and rekordbox's changes are offered as rekordbox's.
        {
            World v;
            v.rekordbox = {rb("1", "Music/A.mp3", {{"OldP", 0}}), rb("2", "Music/B.mp3"),
                           rb("3", "Music/X.mp3", {{"P", 0}}), rb("4", "Music/Y.mp3", {{"P", 1}}),
                           rb("5", "Music/Z.mp3")};
            v.engine = {en("e1", "Music/A.mp3", {{"OldP", 0}}), en("e2", "Music/B.mp3"),
                        en("e3", "Music/X.mp3", {{"P", 0}}), en("e4", "Music/Y.mp3", {{"P", 1}}),
                        en("e5", "Music/Z.mp3")};
            v.rekordboxPlaylists = {{"OldP", false, 8}, {"P", false, 9}};
            v.enginePlaylists = {{"OldP", false, 1}, {"P", false, 1}};
            const auto vbase = baselineOf(v);
            v.engine[1].playlists = {{"OldP", 1}};  // Engine DJ added B since the record
            v.rekordbox[0].playlists.clear();
            v.rekordbox[2].playlists = {{"NewF/P", 0}};
            v.rekordbox[3].playlists.clear();
            v.rekordbox[4].playlists = {{"NewF/P", 1}};
            v.rekordboxPlaylists = {{"NewF", true, 8}, {"NewF/P", false, 9}};
            const auto q = plan(v, vbase);
            assert(q.playlistsToRename.empty() && q.playlistsToCreate.empty() && q.membership.empty());
            assert(q.conflicts.size() == 2 && q.conflicts[1].header.key == "playlist:9");
            const auto listedAsOwn = [](const EngineUpdateProposal &p) {
                return std::any_of(p.engineOwnKept.begin(), p.engineOwnKept.end(),
                                   [](const auto &k) { return k.engineOnlyPlaylist && k.playlistPath == "P"; });
            };
            if (listedAsOwn(q)) {
                std::cerr << "FAIL (h): Engine's \"P\" is listed as Engine's own while its move is refused\n";
            }
            assert(!listedAsOwn(q));
            const auto record = recordAfterSave(vbase, v, q);
            assert(keysOfTheFileGrammar(record));
            assert(recorded(record, 9) && recorded(record, 9)->path == "P");
            if (recorded(record, 9)->members != (Deps{"music/x.mp3", "music/y.mp3"})) {
                std::cerr << "FAIL (h): the record holds \"P\" with " << recorded(record, 9)->members.size()
                          << " member(s), not X and Y\n";
            }
            assert(recorded(record, 9)->members == (Deps{"music/x.mp3", "music/y.mp3"}));
            const auto again = plan(v, record);
            assert(again.conflicts.size() == 2 && !listedAsOwn(again));
            // Engine DJ renames OldP out of the way.
            World cleared = v;
            for (auto &pl : cleared.enginePlaylists) {
                if (pl.path == "OldP") {
                    pl.path = "Kept";
                }
            }
            for (auto &t : cleared.engine) {
                for (auto &m : t.playlists) {
                    if (m.name == "OldP") {
                        m.name = "Kept";
                    }
                }
            }
            const auto later = plan(cleared, recordAfterSave(record, v, again));
            assert(later.conflicts.empty());
            assert(later.playlistsToRename.size() == 1 && later.playlistsToRename[0].fromPath == "P"
                   && later.playlistsToRename[0].toPath == "NewF/P");
            std::multiset<std::string> edits;
            for (const auto &m : later.membership) {
                assert(m.header.checkedByDefault
                       && (m.header.reason == EngineUpdateReason::RekordboxRemoved
                           || m.header.reason == EngineUpdateReason::RekordboxAdded));
                edits.insert((m.kind == MembershipEdit::Kind::Add ? "+" : "-") + m.pathKey);
            }
            for (const auto &k : later.engineOwnKept) {
                if (k.playlistPath == "NewF/P") {
                    std::cerr << "FAIL (h): \"" << k.header.reasonText << "\"\n";
                }
            }
            assert((edits == std::multiset<std::string>{"-music/y.mp3", "+music/z.mp3"}));
        }
        // i: as g, but rekordbox moved "OldF/X" out to "X" rather than
        //    deleting it: X's move is offered, and the refusal says a save
        //    with it ticked clears the way, not to delete or rename the
        //    folder in Engine DJ. After that save the pair is plain.
        {
            World v;
            v.rekordbox = {rb("1", "Music/A.mp3", {{"OldF/X", 0}}), rb("2", "Music/B.mp3")};
            v.engine = {en("e1", "Music/A.mp3", {{"OldF/X", 0}}), en("e2", "Music/B.mp3")};
            v.rekordboxPlaylists = {{"OldF", true, 7}, {"OldF/X", false, 11}};
            v.enginePlaylists = {{"OldF", true, 1}, {"OldF/X", false, 1}};
            const auto vbase = baselineOf(v);
            v.rekordbox[0].playlists = {{"X", 0}};
            v.rekordbox[1].playlists = {{"NewP", 0}};
            v.rekordboxPlaylists = {{"X", false, 11}, {"NewP", false, 7}};
            const auto q = plan(v, vbase);
            assert(q.playlistsToCreate.empty() && q.playlistsToDelete.empty());
            assert(q.playlistsToRename.size() == 1 && q.playlistsToRename[0].fromPath == "OldF/X"
                   && q.playlistsToRename[0].toPath == "X");
            assert(q.conflicts.size() == 1 && q.conflicts[0].header.key == "playlist:7");
            std::cout << "  #124 i: " << q.conflicts[0].header.reasonText << "\n";
            assert(q.conflicts[0].header.reasonText
                   == "rekordbox gave the id of the folder \"OldF\" to the new playlist \"NewP\", so it is no "
                      "rename; Engine's \"OldF\" still holds what rekordbox moved out of it or deleted from it, "
                      "offered here, so Seabass does not delete the folder yet: save with those ticked, then sync "
                      "again");
            World after = v;
            apply(after, q);
            const auto record = recordAfterSave(vbase, v, q);
            const auto next = plan(after, record);
            assert(next.conflicts.empty() && next.playlistsToRename.empty());
            assert(next.playlistsToDelete.size() == 1 && next.playlistsToDelete[0].header.key == "reused:7:OldF");
            assert(next.playlistsToCreate.size() == 1 && next.playlistsToCreate[0].path == "NewP");
        }
        // j: the recorded playlist "OldP" (id 8, A), as Engine has it, given
        //    to the new folder "P/NewF" inside the recorded folder "P" (id
        //    20), which Engine DJ deleted since the record. No save makes P
        //    on its own, so a refusal would come back at every sync: a
        //    conflict under the id the DJ can answer. Unanswered, the record
        //    keeps OldP and it is asked again. rekordbox's side deletes OldP
        //    (under its reused: key) and makes P/NewF with its folder;
        //    Engine's side leaves Engine as it is. Either way the next
        //    analysis asks nothing about it.
        {
            World v;
            v.rekordbox = {rb("1", "Music/A.mp3", {{"OldP", 0}})};
            v.engine = {en("e1", "Music/A.mp3", {{"OldP", 0}})};
            v.rekordboxPlaylists = {{"P", true, 20}, {"OldP", false, 8}};
            v.enginePlaylists = {{"P", true, 1}, {"OldP", false, 1}};
            const auto vbase = baselineOf(v);
            v.enginePlaylists = {{"OldP", false, 1}};  // Engine DJ deleted P
            v.rekordbox[0].playlists.clear();
            v.rekordboxPlaylists = {{"P", true, 20}, {"P/NewF", true, 8}};
            const auto q = plan(v, vbase);
            assert(q.playlistsToRename.empty() && q.playlistsToDelete.empty() && q.playlistsToCreate.empty());
            const auto conflictOf = [](const EngineUpdateProposal &p) -> const EngineUpdateConflict * {
                for (const auto &c : p.conflicts) {
                    if (c.header.key == "playlist:8") {
                        return &c;
                    }
                }
                return nullptr;
            };
            const EngineUpdateConflict *c = conflictOf(q);
            assert(c);
            std::cout << "  #124 j: " << c->header.reasonText << "\n    [" << c->rekordboxSide << "] ["
                      << c->engineSide << "]\n";
            if (c->rekordboxChoice.empty()) {
                std::cerr << "FAIL (j): a refusal that comes back at every sync\n";
            }
            assert(c->rekordboxChoice.size() == 2 && c->engineChoice.empty());
            const auto *del = std::get_if<PlaylistDelete>(&c->rekordboxChoice[0]);
            const auto *make = std::get_if<PlaylistCreate>(&c->rekordboxChoice[1]);
            assert(del && del->path == "OldP" && del->header.key == "reused:8:OldP");
            assert(make && make->path == "P/NewF" && make->folder);
            assert(c->header.reasonText
                   == "rekordbox gave the id of the playlist \"OldP\" to the new folder \"P/NewF\", so it is no "
                      "rename; its folder \"P\" is not in Engine, so creating it makes the folder too");
            // Unanswered: the record keeps OldP, and it is asked again.
            const auto open = recordAfterSave(vbase, v, q);
            assert(recorded(open, 8) && recorded(open, 8)->path == "OldP" && !recorded(open, 8)->folder);
            const auto again = plan(v, open);
            assert(conflictOf(again) && !conflictOf(again)->rekordboxChoice.empty());
            {
                const auto a = answered(q, "playlist:8", true);
                World after = v;
                apply(after, a);
                after.enginePlaylists.push_back({"P", true, 1});  // the create makes its folder
                assert(!hasPlaylist(after, "OldP") && hasPlaylist(after, "P/NewF"));
                const auto record = recordAfterSave(vbase, v, a);
                assert(keysOfTheFileGrammar(record));
                assert(recorded(record, 8) && recorded(record, 8)->path == "P/NewF" && recorded(record, 8)->folder);
                const auto next = plan(after, record);
                assert(next.empty() && next.conflicts.empty());
            }
            {
                const auto e = answered(q, "playlist:8", false);
                World after = v;
                apply(after, e);
                assert(hasPlaylist(after, "OldP") && !hasPlaylist(after, "P/NewF"));
                const auto record = recordAfterSave(vbase, v, e);
                assert(recorded(record, 8) && recorded(record, 8)->path == "P/NewF");
                const auto next = plan(after, record);
                for (const auto &n : next.conflicts) {
                    std::cerr << "FAIL (j): after Engine's side, \"" << n.header.reasonText << "\"\n";
                }
                assert(next.conflicts.empty() && next.playlistsToDelete.empty() && next.playlistsToCreate.empty());
            }
        }
        // k: as j, with rekordbox's new folder "P/NewF/C" (id 9) inside the
        //    new folder, holding the new playlist "P/NewF/C/D" (id 10, B).
        //    Both rows name the row about "P/NewF", the one with buttons,
        //    never the refusal in between, and say what each answer to it
        //    means: rekordbox's way, they come at the next sync; Engine's
        //    way, not until "P/NewF" is in Engine. Answered rekordbox's way
        //    and saved, the next analysis creates both.
        {
            World v;
            v.rekordbox = {rb("1", "Music/A.mp3", {{"OldP", 0}}), rb("2", "Music/B.mp3")};
            v.engine = {en("e1", "Music/A.mp3", {{"OldP", 0}}), en("e2", "Music/B.mp3")};
            v.rekordboxPlaylists = {{"P", true, 20}, {"OldP", false, 8}};
            v.enginePlaylists = {{"P", true, 1}, {"OldP", false, 1}};
            const auto vbase = baselineOf(v);
            v.enginePlaylists = {{"OldP", false, 1}};  // Engine DJ deleted P
            v.rekordbox[0].playlists.clear();
            v.rekordbox[1].playlists = {{"P/NewF/C/D", 0}};
            v.rekordboxPlaylists = {
                {"P", true, 20}, {"P/NewF", true, 8}, {"P/NewF/C", true, 9}, {"P/NewF/C/D", false, 10}};
            const auto q = plan(v, vbase);
            const auto rowOf = [&](const std::string &key) -> const EngineUpdateConflict * {
                for (const auto &c : q.conflicts) {
                    if (c.header.key == key) {
                        return &c;
                    }
                }
                return nullptr;
            };
            for (const auto &[key, what] : std::vector<std::pair<std::string, std::string>>{
                     {"playlist:9", "folder"}, {"playlist:10", "playlist"}}) {
                const EngineUpdateConflict *row = rowOf(key);
                assert(row && row->rekordboxChoice.empty() && row->engineChoice.empty());
                std::cout << "  #124 k (" << key << "): " << row->header.reasonText << "\n";
                assert(row->header.reasonText
                       == "this " + what
                           + " waits on the row about \"P/NewF\": answered rekordbox's way, \"P/NewF\" is made by "
                             "that save and this comes at the next sync; answered Engine's way, it does not come "
                             "until \"P/NewF\" is in Engine");
            }
            const auto a = answered(q, "playlist:8", true);
            World after = v;
            apply(after, a);
            after.enginePlaylists.push_back({"P", true, 1});  // the create makes its folder
            const auto next = plan(after, recordAfterSave(vbase, v, a));
            assert(next.conflicts.empty());
            std::set<std::string> made;
            for (const auto &c : next.playlistsToCreate) {
                made.insert(c.path);
            }
            assert((made == std::set<std::string>{"P/NewF/C", "P/NewF/C/D"}));
        }
        std::cout << "an id whose kind changed is never a rename: delete and create, the create alone, or a refusal "
                     "(#124) OK\n";
    }


    // No baseline: an Engine-only playlist beside a new rekordbox one,
    // holding the same tracks, may be it renamed. One conflict: rename
    // it, or keep it and create rekordbox's beside it.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {{"New", 0}}), rb("2", "Music/B.mp3", {{"New", 1}})};
        w.engine = {en("e1", "Music/A.mp3", {{"Old", 0}}), en("e2", "Music/B.mp3", {{"Old", 1}})};
        w.rekordboxPlaylists = {{"New", false, 3}};
        w.enginePlaylists = {{"Old", false, 1}};
        w.importKey = {{"e1", 1}, {"e2", 1}};
        const auto p = plan(w, std::nullopt);
        assert(p.conflicts.size() == 1);
        const auto &c = p.conflicts[0];
        assert(c.header.key == "playlist:3" && c.header.reason == EngineUpdateReason::NoBaselineRenameGuess);
        assert(c.rekordboxSide == "Rename \"Old\" to \"New\" in Engine");
        assert(c.engineSide == "Keep \"Old\" and create \"New\"");
        assert(std::get<PlaylistRename>(c.rekordboxChoice.at(0)).toPath == "New");
        assert(std::get<PlaylistCreate>(c.engineChoice.at(0)).path == "New");
        std::cout << "no baseline: a rename guess is a conflict with two concrete choices OK\n";
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
        // A refusal has no buttons: its reason says what to do.
        assert(c.rekordboxSide.empty() && c.engineSide.empty());
        assert(p.membership.empty() && p.playlistsToCreate.empty() && p.engineOwnKept.empty());
        std::cout << "ambiguous Engine path conflict OK\n";
    }

    // Two rekordbox playlists spelled "Playlist" at the top level (ids 4
    // and 14), Engine has none there, no baseline: one conflict for the
    // path, keyed by the lowest id, and nothing else planned for it.
    // rekordbox's choice is one playlist holding the union, list 4 first.
    const auto sharedWorld = [] {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {{"Playlist", 0, 4}}),
                       rb("2", "Music/B.mp3", {{"Playlist", 1, 4}, {"Playlist", 1, 14}}),
                       rb("3", "Music/C.mp3", {{"Playlist", 0, 14}}), rb("4", "Music/D.mp3", {{"Playlist", 2, 14}}),
                       rb("5", "Music/E.mp3")};
        w.engine = {en("e1", "Music/A.mp3"), en("e2", "Music/B.mp3"), en("e3", "Music/C.mp3"),
                    en("e4", "Music/D.mp3"), en("e5", "Music/E.mp3")};
        w.rekordboxPlaylists = {{"Playlist", false, 4}, {"Playlist", false, 14}};
        return w;
    };
    const std::string sharedText =
        "rekordbox has 2 playlists spelled \"Playlist\" here; Engine can hold one at this path";
    {
        const World w = sharedWorld();
        const auto p = plan(w, std::nullopt);
        assert(p.conflicts.size() == 1);
        const auto &c = p.conflicts[0];
        assert(c.header.key == "playlist:4" && c.header.conflict && !c.header.checkedByDefault);
        assert(c.header.reason == EngineUpdateReason::RekordboxPathShared);
        assert(c.header.reasonText == sharedText);
        assert(c.rekordboxSide == "Create one Engine playlist \"Playlist\" holding all 2 of them");
        assert(c.engineSide == "Leave Engine without \"Playlist\"");
        assert(c.engineChoice.empty());
        assert(c.rekordboxChoice.size() == 5);
        const auto &create = std::get<PlaylistCreate>(c.rekordboxChoice[0]);
        assert(create.path == "Playlist" && create.pdbId == 4 && !create.folder);
        const Deps unionOrder = {"music/a.mp3", "music/b.mp3", "music/c.mp3", "music/d.mp3"};
        std::string after;
        for (std::size_t i = 0; i < unionOrder.size(); ++i) {
            const auto &add = std::get<MembershipEdit>(c.rekordboxChoice[i + 1]);
            assert(add.kind == MembershipEdit::Kind::Add && add.playlistPath == "Playlist");
            assert(add.pathKey == unionOrder[i] && add.afterPathKey == after);
            assert(add.header.key == "member:4:" + unionOrder[i]);
            after = add.pathKey;
        }
        assert(p.playlistsToCreate.empty() && p.membership.empty() && p.playlistsToDelete.empty());
        assert(p.playlistsToRename.empty() && p.engineOwnKept.empty());
        World after_ = w;
        apply(after_, p, true);
        assert(engineOrder(after_, "Playlist") == unionOrder);
        std::cout << "two rekordbox playlists at one path, one conflict OK\n";
    }

    // The same with an Engine "Playlist" (E, B): still one conflict, no
    // adds planned for it; the choice adds the union around what Engine
    // holds.
    {
        World w = sharedWorld();
        w.engine[4].playlists = {{"Playlist", 0}};
        w.engine[1].playlists = {{"Playlist", 1}};
        w.enginePlaylists = {{"Playlist", false, 1}};
        const auto p = plan(w, std::nullopt);
        assert(p.conflicts.size() == 1);
        const auto &c = p.conflicts[0];
        assert(c.header.key == "playlist:4" && c.header.reason == EngineUpdateReason::RekordboxPathShared);
        assert(c.header.reasonText == sharedText);
        assert(c.rekordboxSide == "Add the tracks of all 2 of them to Engine's \"Playlist\"");
        assert(c.engineSide == "Leave Engine's \"Playlist\" as it is");
        assert(c.rekordboxChoice.size() == 3);
        const auto &a = std::get<MembershipEdit>(c.rekordboxChoice[0]);
        const auto &cc = std::get<MembershipEdit>(c.rekordboxChoice[1]);
        const auto &d = std::get<MembershipEdit>(c.rekordboxChoice[2]);
        assert(a.pathKey == "music/a.mp3" && a.afterPathKey.empty());
        assert(cc.pathKey == "music/c.mp3" && cc.afterPathKey == "music/b.mp3");
        assert(d.pathKey == "music/d.mp3" && d.afterPathKey == "music/c.mp3");
        assert(p.playlistsToCreate.empty() && p.membership.empty() && p.engineOwnKept.empty());
        World after = w;
        apply(after, p, true);
        assert((engineOrder(after, "Playlist")
                == Deps{"music/a.mp3", "music/e.mp3", "music/b.mp3", "music/c.mp3", "music/d.mp3"}));
        std::cout << "two rekordbox playlists at one Engine path, one conflict OK\n";
    }

    // A baseline that knows id 4 as Engine's "Playlist": id 4 is planned
    // as any playlist (E added), id 14 alone is the conflict, its choice
    // after what id 4 puts there.
    {
        World w = sharedWorld();
        w.rekordbox = {rb("1", "Music/A.mp3", {{"Playlist", 0, 4}}), rb("2", "Music/B.mp3", {{"Playlist", 1, 4}}),
                       rb("3", "Music/C.mp3"), rb("4", "Music/D.mp3"), rb("5", "Music/E.mp3")};
        w.engine[0].playlists = {{"Playlist", 0}};
        w.engine[1].playlists = {{"Playlist", 1}};
        w.rekordboxPlaylists = {{"Playlist", false, 4}};
        w.enginePlaylists = {{"Playlist", false, 1}};
        const auto base = baselineOf(w);
        w.rekordbox = {rb("1", "Music/A.mp3", {{"Playlist", 0, 4}}), rb("2", "Music/B.mp3", {{"Playlist", 1, 4}}),
                       rb("3", "Music/C.mp3", {{"Playlist", 0, 14}}), rb("4", "Music/D.mp3", {{"Playlist", 1, 14}}),
                       rb("5", "Music/E.mp3", {{"Playlist", 2, 4}})};
        w.rekordboxPlaylists = {{"Playlist", false, 4}, {"Playlist", false, 14}};
        const auto p = plan(w, base);
        assert(p.membership.size() == 1);
        const auto &e = p.membership[0];
        assert(e.kind == MembershipEdit::Kind::Add && e.header.key == "member:4:music/e.mp3");
        assert(e.afterPathKey == "music/b.mp3" && e.header.reason == EngineUpdateReason::RekordboxAdded);
        assert(p.conflicts.size() == 1);
        const auto &c = p.conflicts[0];
        assert(c.header.key == "playlist:14" && c.header.reason == EngineUpdateReason::RekordboxPathShared);
        assert(c.header.reasonText == sharedText);
        assert(c.rekordboxChoice.size() == 2);
        const auto &cc = std::get<MembershipEdit>(c.rekordboxChoice[0]);
        const auto &d = std::get<MembershipEdit>(c.rekordboxChoice[1]);
        assert(cc.pathKey == "music/c.mp3" && cc.afterPathKey == "music/e.mp3" && cc.header.key == "member:14:music/c.mp3");
        assert(d.pathKey == "music/d.mp3" && d.afterPathKey == "music/c.mp3");
        assert(p.playlistsToCreate.empty() && p.playlistsToDelete.empty() && p.engineOwnKept.empty());
        std::cout << "two rekordbox playlists at one path, the baseline pairs one OK\n";
    }

    // A rename onto a path a sibling already spells: the same conflict,
    // no rename, and Engine's playlist at the old path is not "Engine's
    // own".
    {
        World w = sharedWorld();
        w.rekordbox = {rb("1", "Music/A.mp3", {{"Playlist", 0, 4}}), rb("2", "Music/B.mp3"),
                       rb("3", "Music/C.mp3", {{"Other", 0, 14}}), rb("4", "Music/D.mp3"), rb("5", "Music/E.mp3")};
        w.engine[0].playlists = {{"Playlist", 0}};
        w.engine[2].playlists = {{"Other", 0}};
        w.rekordboxPlaylists = {{"Playlist", false, 4}, {"Other", false, 14}};
        w.enginePlaylists = {{"Playlist", false, 1}, {"Other", false, 1}};
        const auto base = baselineOf(w);
        w.rekordbox[2].playlists = {{"Playlist", 0, 14}};
        w.rekordboxPlaylists = {{"Playlist", false, 4}, {"Playlist", false, 14}};
        const auto p = plan(w, base);
        assert(p.playlistsToRename.empty() && p.playlistsToCreate.empty() && p.membership.empty());
        assert(p.conflicts.size() == 1);
        const auto &c = p.conflicts[0];
        assert(c.header.key == "playlist:14" && c.header.reason == EngineUpdateReason::RekordboxPathShared);
        assert(c.header.reasonText == sharedText);
        assert(c.rekordboxChoice.size() == 1);
        const auto &cc = std::get<MembershipEdit>(c.rekordboxChoice[0]);
        assert(cc.pathKey == "music/c.mp3" && cc.afterPathKey == "music/a.mp3" && cc.playlistPath == "Playlist");
        assert(p.engineOwnKept.empty() && p.playlistsToDelete.empty());
        std::cout << "rename onto a sibling's path is a conflict OK\n";
    }

    // Declining it keeps it out until one of the member lists changes;
    // what depends on its key goes with it.
    {
        World w = sharedWorld();
        w.rekordboxPlaylists = {};
        for (auto &t : w.rekordbox) {
            t.playlists.clear();
        }
        auto base = baselineOf(w);  // knows the tracks, neither playlist
        w = sharedWorld();
        w.rekordboxPlaylists.push_back({"Playlist/Sub", false, 20});
        w.rekordbox[4].playlists = {{"Playlist/Sub", 0, 20}};
        const auto first = plan(w, base);
        assert(first.conflicts.size() == 1 && first.conflicts[0].header.key == "playlist:4");
        // What sits under the path waits for the conflict.
        assert(first.playlistsToCreate.size() == 1 && first.playlistsToCreate[0].path == "Playlist/Sub");
        assert(first.playlistsToCreate[0].header.dependsOn == Deps{"playlist:4"});
        assert(first.membership.size() == 1);
        assert((first.membership[0].header.dependsOn == Deps{"playlist:4", "playlist:20"}));
        base.declined["playlist:4"] = first.conflicts[0].header.rekordboxState;
        const auto second = plan(w, base);
        assert(second.conflicts.empty() && second.playlistsToCreate.empty() && second.membership.empty());
        assert((second.declinedSuppressed
                == Deps{"member:20:music/e.mp3", "playlist:20", "playlist:4"}));
        // List 14 gains E: the conflict is back.
        w.rekordbox[4].playlists.push_back({"Playlist", 3, 14});
        const auto third = plan(w, base);
        assert(third.conflicts.size() == 1 && third.conflicts[0].header.key == "playlist:4");
        assert(third.conflicts[0].header.rekordboxState != first.conflicts[0].header.rekordboxState);
        std::cout << "declined shared path stays out until a list changes OK\n";
    }

    // Two folders spelled "F": one folder, the lowest id, and what each
    // holds under it planned as usual.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {{"F/X", 0, 7}}), rb("2", "Music/B.mp3", {{"F/Y", 0, 8}})};
        w.engine = {en("e1", "Music/A.mp3"), en("e2", "Music/B.mp3")};
        w.rekordboxPlaylists = {{"F", true, 5}, {"F", true, 6}, {"F/X", false, 7}, {"F/Y", false, 8}};
        const auto p = plan(w, std::nullopt);
        assert(p.conflicts.empty());
        assert(p.playlistsToCreate.size() == 3);
        assert(p.playlistsToCreate[0].path == "F" && p.playlistsToCreate[0].pdbId == 5);
        assert(p.playlistsToCreate[0].folder);
        assert(p.membership.size() == 2);
        World after = w;
        apply(after, p);
        assert(engineOrder(after, "F/X") == Deps{"music/a.mp3"} && engineOrder(after, "F/Y") == Deps{"music/b.mp3"});
        std::cout << "two rekordbox folders at one path are one folder OK\n";
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

        // Without a baseline the same reorder is no conflict: the order
        // is rekordbox's to decide, every move under the playlist's
        // order item, checked.
        const auto q = plan(w, std::nullopt);
        assert(q.conflicts.empty() && q.membership.size() == 2);
        for (const auto &m : q.membership) {
            assert(m.header.key == "order:1" && m.header.checkedByDefault && !m.header.conflict);
            assert(m.header.reason == EngineUpdateReason::NoBaselineOrder);
            assert(m.header.reasonText
                   == "1 track sits at the top in rekordbox and at the bottom in Engine; Engine takes rekordbox's "
                      "order");
        }
        World afterQ = w;
        apply(afterQ, q);
        assert((engineOrder(afterQ, "P") == Deps{"music/c.mp3", "music/a.mp3", "music/b.mp3"}));
        assert(plan(afterQ, std::nullopt).empty());
        std::cout << "same-set reorder OK\n";
    }

    // The order of a playlist both sides hold whole, without a record of
    // it: Engine takes rekordbox's, and the row says where the members
    // that move sit on each side. One run in both, in the middle: by
    // position over the whole playlist. Not one run: how many move.
    {
        const auto world = [](const std::vector<std::string> &rOrder, const std::vector<std::string> &eOrder) {
            World w;
            for (std::size_t i = 0; i < rOrder.size(); ++i) {
                w.rekordbox.push_back(rb(std::to_string(i + 1), "Music/" + rOrder[i] + ".mp3",
                                         {{"P", static_cast<int>(i)}}));
            }
            for (std::size_t i = 0; i < eOrder.size(); ++i) {
                w.engine.push_back(en("e" + std::to_string(i + 1), "Music/" + eOrder[i] + ".mp3",
                                      {{"P", static_cast<int>(i)}}));
            }
            w.rekordboxPlaylists = {{"P", false, 1}};
            w.enginePlaylists = {{"P", false, 1}};
            return w;
        };
        const auto lower = [](const std::vector<std::string> &names) {
            Deps out;
            for (const auto &name : names) {
                out.push_back(lowerKey("music/" + name + ".mp3"));
            }
            return out;
        };
        const auto check = [&](const std::vector<std::string> &rOrder, const std::vector<std::string> &eOrder,
                               std::size_t moves, const std::string &text) {
            const World w = world(rOrder, eOrder);
            const auto p = plan(w, std::nullopt);
            assert(p.conflicts.empty() && p.membership.size() == 2 * moves);
            for (std::size_t i = 0; i < p.membership.size(); ++i) {
                const auto &m = p.membership[i];
                assert(m.kind == (i < moves ? MembershipEdit::Kind::Remove : MembershipEdit::Kind::Add));
                assert(m.header.key == "order:1" && m.header.checkedByDefault && !m.header.conflict);
                assert(m.header.reason == EngineUpdateReason::NoBaselineOrder);
                if (m.header.reasonText != text) {
                    std::cerr << "reason: " << m.header.reasonText << "\n";
                }
                assert(m.header.reasonText == text);
            }
            World after = w;
            apply(after, p);
            assert(engineOrder(after, "P") == lower(rOrder));
            assert(plan(after, std::nullopt).empty());
        };
        check({"A", "B", "X", "Y", "C", "D", "E", "F"}, {"A", "B", "C", "D", "E", "X", "Y", "F"}, 2,
              "2 tracks sit at positions 3 to 4 in rekordbox and 6 to 7 in Engine; Engine takes rekordbox's order");
        check({"A", "X", "B", "C", "D"}, {"A", "B", "C", "D", "X"}, 1,
              "1 track sits at position 2 in rekordbox and at the bottom in Engine; Engine takes rekordbox's order");
        check({"A", "B", "C", "D", "E", "F"}, {"B", "A", "C", "D", "F", "E"}, 2,
              "The two orders differ (2 tracks move); Engine takes rekordbox's order");
        std::cout << "a whole order without a record: where the moves sit OK\n";
    }

    // Members both sides hold that the record lacks, in another place on
    // each side, the recorded members in the recorded order on both: no
    // record says where they belong, and rekordbox decides the order.
    // (WS_NEW's "Spacy Techno": 29 tracks at the top in rekordbox, at the
    // bottom in Engine, none of them in the record.)
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {{"P", 0}}), rb("2", "Music/B.mp3", {{"P", 1}}),
                       rb("3", "Music/C.mp3", {{"P", 2}})};
        w.engine = {en("e1", "Music/A.mp3", {{"P", 0}}), en("e2", "Music/B.mp3", {{"P", 1}}),
                    en("e3", "Music/C.mp3", {{"P", 2}})};
        w.rekordboxPlaylists = {{"P", false, 1}};
        w.enginePlaylists = {{"P", false, 1}};
        const auto base = baselineOf(w);
        w.rekordbox = {rb("1", "Music/A.mp3", {{"P", 2}}), rb("2", "Music/B.mp3", {{"P", 3}}),
                       rb("3", "Music/C.mp3", {{"P", 4}}), rb("4", "Music/X.mp3", {{"P", 0}}),
                       rb("5", "Music/Y.mp3", {{"P", 1}})};
        w.engine = {en("e1", "Music/A.mp3", {{"P", 0}}), en("e2", "Music/B.mp3", {{"P", 1}}),
                    en("e3", "Music/C.mp3", {{"P", 2}}), en("e4", "Music/X.mp3", {{"P", 3}}),
                    en("e5", "Music/Y.mp3", {{"P", 4}})};
        const auto p = plan(w, base);
        assert(p.conflicts.empty() && p.membership.size() == 4);
        // The removes, then the adds in rekordbox's order, each after the
        // member before it, all under the playlist's order item.
        for (std::size_t i = 0; i < 4; ++i) {
            const auto &m = p.membership[i];
            assert(m.kind == (i < 2 ? MembershipEdit::Kind::Remove : MembershipEdit::Kind::Add));
            assert(m.header.key == "order:1" && m.playlistPath == "P");
            assert(m.header.checkedByDefault && !m.header.conflict);
            assert(m.header.reason == EngineUpdateReason::NoBaselineOrder);
            assert(m.header.reasonText
                   == "2 tracks sit at the top in rekordbox and at the bottom in Engine; Engine takes rekordbox's "
                      "order");
            assert(m.header.dependsOn.empty());
        }
        assert(p.membership[2].pathKey == "music/x.mp3" && p.membership[2].afterPathKey.empty());
        assert(p.membership[3].pathKey == "music/y.mp3" && p.membership[3].afterPathKey == "music/x.mp3");
        World after = w;
        apply(after, p);
        assert((engineOrder(after, "P")
                == Deps{"music/x.mp3", "music/y.mp3", "music/a.mp3", "music/b.mp3", "music/c.mp3"}));
        assert(plan(after, nextBaseline(&base, after.rekordbox, after.rekordboxPlaylists, 14205, {"order:1"},
                                        {"order:1"}, {}, stickRelative, lowerKey))
                   .empty());
        std::cout << "members the record lacks, out of place: rekordbox's order OK\n";
    }

    // Both sides changed the order of the recorded members: Engine takes
    // rekordbox's, checked, every move under the playlist's order item;
    // declined, it stays out while rekordbox's order is the one declined.
    // Only Engine changed it: Engine's own, kept, under the same item.
    {
        World w;
        w.rekordbox = {rb("1", "Music/A.mp3", {{"P", 0}}), rb("2", "Music/B.mp3", {{"P", 1}}),
                       rb("3", "Music/C.mp3", {{"P", 2}}), rb("4", "Music/D.mp3", {{"P", 3}})};
        w.engine = {en("e1", "Music/A.mp3", {{"P", 0}}), en("e2", "Music/B.mp3", {{"P", 1}}),
                    en("e3", "Music/C.mp3", {{"P", 2}}), en("e4", "Music/D.mp3", {{"P", 3}})};
        w.rekordboxPlaylists = {{"P", false, 1}};
        w.enginePlaylists = {{"P", false, 1}};
        const auto base = baselineOf(w);
        w.engine[2].playlists = {{"P", 3}};  // Engine: A B D C
        w.engine[3].playlists = {{"P", 2}};
        const auto own = plan(w, base);
        assert(own.conflicts.empty() && own.membership.empty() && own.engineOwnKept.size() == 1);
        assert(own.engineOwnKept[0].header.key == "order:1");
        assert(own.engineOwnKept[0].header.reasonText
               == "Engine changed the order of \"P\" after Seabass last recorded the stick");

        w.rekordbox[0].playlists = {{"P", 1}};  // rekordbox: B A C D
        w.rekordbox[1].playlists = {{"P", 0}};
        const auto p = plan(w, base);
        assert(p.conflicts.empty() && p.engineOwnKept.empty() && p.membership.size() == 4);
        for (const auto &m : p.membership) {
            assert(m.header.key == "order:1" && m.header.checkedByDefault && !m.header.conflict);
            assert(m.header.reason == EngineUpdateReason::BothChanged);
            assert(m.header.reasonText == "The two orders differ (2 tracks move); Engine takes rekordbox's order");
            assert(m.header.rekordboxState == p.membership[0].header.rekordboxState);
        }
        World after = w;
        apply(after, p);
        assert((engineOrder(after, "P") == Deps{"music/b.mp3", "music/a.mp3", "music/c.mp3", "music/d.mp3"}));
        assert(plan(after, nextBaseline(&base, after.rekordbox, after.rekordboxPlaylists, 14205, {"order:1"},
                                        {"order:1"}, {}, stickRelative, lowerKey))
                   .empty());

        // Declined: the record keeps its order, and the order stays out
        // while rekordbox's is the one declined.
        const auto declined = nextBaseline(&base, w.rekordbox, w.rekordboxPlaylists, 14205, {"order:1"}, {},
                                           {{"order:1", p.membership[0].header.rekordboxState}}, stickRelative,
                                           lowerKey);
        assert((declined.findPlaylist(1)->members
                == std::vector<std::string>{"music/a.mp3", "music/b.mp3", "music/c.mp3", "music/d.mp3"}));
        const auto again = plan(w, declined);
        assert(again.membership.empty() && again.conflicts.empty());
        assert(std::count(again.declinedSuppressed.begin(), again.declinedSuppressed.end(), "order:1") == 1);
        std::cout << "both sides changed the order: rekordbox's, one item OK\n";
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
        assert(p.conflicts[0].rekordboxSide == "Set the rating in Engine to 5 stars (rekordbox's)");
        assert(p.conflicts[0].engineSide == "Keep Engine's 2 stars");
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
        assert(c.rekordboxSide == "Set the rating in Engine to 5 stars (rekordbox's)" && c.engineSide == "Keep Engine's 3 stars");
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
        assert(r.restoresSeabassWrite && "Seabass's own write goes back");
        assert(p.metadataToEngine.size() == 1 && p.metadataToEngine[0].header.key == "rating:music/b.mp3");
        assert(!p.metadataToEngine[0].rating);
        assert(p.metadataToEngine[0].header.reasonText == "rekordbox cleared the rating (4 stars)" + Since);
        assert(p.conflicts.size() == 1 && p.conflicts[0].header.key == "rating:music/c.mp3");
        assert(p.conflicts[0].header.reason == EngineUpdateReason::OriginUnknown);
        assert(p.conflicts[0].rekordboxSide == "Clear the rating in Engine (rekordbox has none)");
        assert(p.conflicts[0].engineSide == "Keep Engine's 4 stars and write the rating back onto rekordbox");
        const auto &back = std::get<MetadataEdit>(p.conflicts[0].engineChoice[0]);
        assert(back.direction == MetadataEdit::Direction::ToRekordbox && back.rating == 4);
        assert(!back.restoresSeabassWrite && "nothing recorded Seabass wrote it: Engine's value over rekordbox's");
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
        assert(c.rekordboxSide.empty() && c.engineSide.empty());
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
        assert(p.conflicts[0].rekordboxSide == "Take it out of \"P\" in Engine");
        assert(p.conflicts[0].engineSide == "Keep it in \"P\"");
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
        assert(p.conflicts[0].rekordboxSide == "Set the comment in Engine to \"rekordbox note\" (rekordbox's)");
        assert(p.conflicts[0].engineSide == "Keep Engine's comment \"another note\"");
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
        assert(c.header.reasonText == "rekordbox set pad 1 to 0:10.300 (was 0:10.000)" + Since);
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
        assert(c.rekordboxSide == "Set pad 1 in Engine to 0:20.000, rekordbox's position");
        assert(c.engineSide == "Keep pad 1 in Engine at 0:30.000");
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

    // Rule B with Engine holding the cue as its cue point alone: until
    // 2026-10-17 Sync Cue Points copied Engine's cue point onto rekordbox
    // as a memory cue (the baseline says Seabass wrote it), and when
    // rekordbox's re-export dropped it, it went back. The cue point is
    // Engine's own now, not a memory cue, so a memory cue that was only
    // ever its copy stays gone: nothing to write, nothing to ask.
    {
        World w = cuePair({hot(1, 10000), mem(30000)}, {hot(1, 10000), mem(30000)});
        auto base = baselineOf(w);
        setOrigin(base, ValueOrigin::Seabass);
        w.rekordbox[0].cues = {hot(1, 10000)};
        const auto p = plan(w, base);
        assert(p.cuesToEngine.empty() && p.conflicts.empty() && p.engineOwnKept.empty());
        assert(p.cuesToRekordbox.empty() && "Engine's cue point is not a memory cue to put back");
        assert(p.empty());
        std::cout << "a memory cue that was Engine's cue point stays dropped OK\n";
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

    // Rule B: nobody recorded who wrote it. A conflict until 2026-10-10;
    // now an empty rekordbox pad never beats an Engine cue
    // (engineCuesOverEmptyPads): Engine's goes back onto rekordbox,
    // checked.
    {
        World w = cuePair({hot(1, 10000), hot(2, 20000)}, {hot(1, 10000), hot(2, 20000)});
        const auto base = baselineOf(w);  // a read: every origin Unknown
        w.rekordbox[0].cues = {hot(1, 10000)};
        const auto p = plan(w, base);
        assert(p.cuesToEngine.empty() && p.conflicts.empty() && p.cuesToRekordbox.size() == 1);
        const auto &c = p.cuesToRekordbox[0];
        assert(c.header.key == "cue:hot:2:music/a.mp3" && c.header.checkedByDefault && !c.header.conflict);
        assert(c.header.reason == EngineUpdateReason::EngineOverEmptyPad);
        assert(c.header.reasonText == "rekordbox has nothing on pad 2; Engine's cue at 0:20.000 goes back");
        assert(c.plan.direction == SyncPlan::Direction::ToA && c.plan.reason == SyncPlan::Reason::EngineOverEmptyPad);
        assert(sameCueList(c.plan.cuesToApply, {hot(1, 10000), hot(2, 20000)}));
        World after = w;
        apply(after, p);
        assert(plan(after, base).empty());
        std::cout << "dropped cue of unknown origin goes back onto rekordbox OK\n";
    }

    // Rule D: Engine changed a pad, rekordbox did not: Engine's own, kept.
    // Engine moving pad 1 is; Engine setting pad 4, which rekordbox holds
    // nothing on, was until 2026-10-10 and now goes back onto rekordbox
    // (an empty rekordbox pad never beats an Engine cue).
    {
        World w = cuePair({hot(1, 10000)}, {hot(1, 10000)});
        const auto base = baselineOf(w);
        w.engine[0].cues = {hot(1, 12000)};
        const auto p = plan(w, base);
        assert(p.empty());
        assert(p.engineOwnKept.size() == 1);
        const auto &k = p.engineOwnKept[0];
        assert(k.header.key == "cue:hot:1:music/a.mp3" && k.header.reason == EngineUpdateReason::EngineOwn);
        assert(k.header.reasonText
               == "Engine set pad 1 to 0:12.000 (was 0:10.000) after Seabass last recorded the stick");
        assert(k.engine.sourceId == "e1");

        World set = cuePair({hot(1, 10000)}, {hot(1, 10000)});
        set.engine[0].cues.push_back(hot(4, 50000));
        const auto q = plan(set, base);
        assert(q.engineOwnKept.empty() && q.conflicts.empty() && q.cuesToRekordbox.size() == 1);
        assert(q.cuesToRekordbox[0].header.reason == EngineUpdateReason::EngineOverEmptyPad);
        assert(q.cuesToRekordbox[0].header.reasonText
               == "rekordbox has nothing on pad 4; Engine's cue at 0:50.000 goes back");
        assert(sameCueList(q.cuesToRekordbox[0].plan.cuesToApply, {hot(1, 10000), hot(4, 50000)}));
        std::cout << "Engine's own cue kept, Engine's new pad goes back OK\n";
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
        assert(c.engineChoice.empty());
        assert(c.rekordboxSide == "Remove pad 5 from Engine (rekordbox no longer has the memory cue at 0:30.000)");
        assert(c.engineSide == "Keep pad 5 in Engine as a hot cue of its own");
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
        // A hot cue of Engine's own on a pad rekordbox holds nothing on:
        // it goes back onto rekordbox (engineCuesOverEmptyPads).
        const auto f = plan(apart, base);
        assert(f.conflicts.empty() && f.cuesToEngine.empty() && f.engineOwnKept.empty());
        assert(f.cuesToRekordbox.size() == 1 && f.cuesToRekordbox[0].header.key == "cue:hot:5:music/a.mp3");
        assert(f.cuesToRekordbox[0].header.reason == EngineUpdateReason::EngineOverEmptyPad);
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
        assert(k.rekordboxSide == "Use rekordbox's cues in Engine" && k.engineSide == "Copy Engine's cues to rekordbox");
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

    // An added track carries its cues as the policy says: rekordbox's
    // export pad 1 at 7 ms and a negative "no cue" sentinel are junk
    // (isJunkCue) and never ride into the new Engine row; the real cues do.
    {
        World w = cuePair({hot(1, 10000)}, {hot(1, 10000)});
        const auto base = baselineOf(w);
        Track n = rb("2", "Music/N.mp3");
        n.cues = {hot(1, 7), mem(-0.5), hot(2, 5000), mem(30000)};
        w.rekordbox.push_back(n);
        const auto p = plan(w, base);
        assert(p.tracksToAdd.size() == 1);
        assert(sameCueList(p.tracksToAdd[0].rekordbox.cues, {hot(2, 5000), mem(30000)}));
        std::cout << "an added track carries no junk cues OK\n";
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

    // rekordbox's export put a cue at 0:00 on pad 3, where Engine holds
    // its cue at 1:07.751 (2026-10-10). Whoever the baseline says wrote
    // pad 3, it is export noise: one checked write onto rekordbox with
    // Engine's cue over the 0:00 one, no conflict, nothing onto Engine.
    // Before, an unknown origin asked (OriginUnknown), rekordbox's origin
    // took Engine's cue off (RekordboxRemoved), and Seabass's restored it.
    {
        const std::string text =
            "rekordbox's export put a cue at 0:00 on pad 3 over the cue Engine has at 1:07.751; Engine's goes back";
        for (const ValueOrigin origin : {ValueOrigin::Unknown, ValueOrigin::Rekordbox, ValueOrigin::Seabass}) {
            World w = cuePair({hot(1, 10000), hot(3, 67751)}, {hot(1, 10000), hot(3, 67751)});
            auto base = baselineOf(w);
            setOrigin(base, origin);
            w.rekordbox[0].cues = {hot(1, 10000), hot(3, 0)};
            const auto p = plan(w, base);
            assert(p.conflicts.empty() && p.cuesToEngine.empty() && p.engineOwnKept.empty());
            assert(p.cuesToRekordbox.size() == 1);
            const auto &c = p.cuesToRekordbox[0];
            assert(c.header.key == "cue:hot:3:music/a.mp3" && c.header.checkedByDefault && !c.header.conflict);
            assert((itemKeys(c.header) == Deps{"cue:hot:3:music/a.mp3"}));
            assert(c.header.reason == EngineUpdateReason::StartCueOverEngine);
            assert(c.header.reasonText == text);
            assert(c.plan.direction == SyncPlan::Direction::ToA && !c.plan.needsChoice);
            assert(c.plan.reason == SyncPlan::Reason::StartCueOverEngine);
            assert(sameCueList(c.plan.cuesToApply, {hot(1, 10000), hot(3, 67751)}));
            assert(p.onlyCues());
            World after = w;
            apply(after, p);
            assert(plan(after, base).empty());
            assert(plan(after, baselineFrom(after.rekordbox, after.rekordboxPlaylists, 15132, stickRelative, lowerKey))
                       .empty());
        }

        // Engine set pad 3 on the deck after the record (B equals R, which
        // was Engine's own and kept): with rekordbox's 0:00 cue over it,
        // Engine's goes onto rekordbox.
        World own = cuePair({hot(1, 10000)}, {hot(1, 10000)});
        const auto ownBase = baselineOf(own);
        own.engine[0].cues.push_back(hot(3, 67751));
        own.rekordbox[0].cues.push_back(hot(3, 0));
        const auto q = plan(own, ownBase);
        assert(q.conflicts.empty() && q.engineOwnKept.empty() && q.cuesToRekordbox.size() == 1);
        assert(q.cuesToRekordbox[0].header.reason == EngineUpdateReason::StartCueOverEngine);
        assert(sameCueList(q.cuesToRekordbox[0].plan.cuesToApply, {hot(1, 10000), hot(3, 67751)}));
        World ownAfter = own;
        apply(ownAfter, q);
        assert(plan(ownAfter, ownBase).empty());
        std::cout << "rekordbox's 0:00 pad over Engine's cue: Engine's goes back OK\n";
    }

    // Where Engine holds no cue on the pad, the 0:00 cue stays ignored: not
    // copied onto Engine, nothing written.
    {
        World w = cuePair({hot(1, 10000)}, {hot(1, 10000)});
        const auto base = baselineOf(w);
        w.rekordbox[0].cues.push_back(hot(3, 0));
        const auto p = plan(w, base);
        assert(p.empty() && p.engineOwnKept.empty());
        assert(plan(w, std::nullopt).empty());
        std::cout << "0:00 pad over an empty Engine pad stays ignored OK\n";
    }

    // A memory cue at 0:00 has no pad: junk under the policy, left out as
    // before, so pad 3 is one rekordbox holds nothing on, not one it put a
    // 0:00 cue over: Engine's goes back as an empty pad's
    // (EngineOverEmptyPad), or comes off Engine where the record says
    // rekordbox put it there.
    {
        World w = cuePair({hot(1, 10000), hot(3, 67751)}, {hot(1, 10000), hot(3, 67751)});
        auto base = baselineOf(w);  // every origin Unknown
        w.rekordbox[0].cues = {hot(1, 10000), mem(0)};
        const auto p = plan(w, base);
        assert(p.conflicts.empty() && p.cuesToRekordbox.size() == 1);
        assert(p.cuesToRekordbox[0].header.reason == EngineUpdateReason::EngineOverEmptyPad);
        setOrigin(base, ValueOrigin::Rekordbox);
        const auto q = plan(w, base);
        assert(q.cuesToRekordbox.empty() && q.conflicts.empty() && q.cuesToEngine.size() == 1);
        assert(q.cuesToEngine[0].header.reason == EngineUpdateReason::RekordboxRemoved);
        std::cout << "memory cue at 0:00 is not a pad over Engine's OK\n";
    }

    // With "Ignore cues at 0:00" off the cue at 0:00 is a cue: rekordbox
    // moved pad 3 there, and that goes to Engine, as before.
    {
        World w = cuePair({hot(1, 10000), hot(3, 67751)}, {hot(1, 10000), hot(3, 67751)});
        const auto base = baselineOf(w);
        w.rekordbox[0].cues = {hot(1, 10000), hot(3, 0)};
        MatchingPolicy::set(2.0, 10.0, false);
        const auto p = plan(w, base);
        MatchingPolicy::reset();
        assert(p.cuesToRekordbox.empty() && p.conflicts.empty() && p.cuesToEngine.size() == 1);
        assert(p.cuesToEngine[0].header.reason == EngineUpdateReason::RekordboxChanged);
        assert(p.cuesToEngine[0].header.reasonText == "rekordbox set pad 3 to 0:00.000 (was 1:07.751)" + Since);
        assert(sameCueList(p.cuesToEngine[0].plan.cuesToApply, {hot(1, 10000), hot(3, 0)}));
        std::cout << "0:00 pad with the preference off is rekordbox's change OK\n";
    }

    // With a restore: one write onto rekordbox carries both, the restore's
    // reason first. With a conflict beside it: the conflict depends on the
    // write, and its choice onto rekordbox keeps Engine's pad 3.
    {
        World w = cuePair({hot(1, 10000), hot(2, 20000), hot(3, 67751)},
                          {hot(1, 10000), hot(2, 20000), hot(3, 67751)});
        auto base = baselineOf(w);
        setOrigin(base, ValueOrigin::Seabass);
        w.rekordbox[0].cues = {hot(1, 10000), hot(3, 0)};
        const auto p = plan(w, base);
        assert(p.conflicts.empty() && p.cuesToEngine.empty() && p.cuesToRekordbox.size() == 1);
        const auto &c = p.cuesToRekordbox[0];
        assert((itemKeys(c.header) == Deps{"cue:hot:2:music/a.mp3", "cue:hot:3:music/a.mp3"}));
        assert(c.header.reason == EngineUpdateReason::ExportDropped);
        assert(c.header.reasonText
               == "rekordbox's export dropped 1 cue Seabass had synced from Engine; it goes back; rekordbox's export "
                  "put a cue at 0:00 on pad 3 over the cue Engine has at 1:07.751; Engine's goes back");
        assert(sameCueList(c.plan.cuesToApply, {hot(1, 10000), hot(3, 67751), hot(2, 20000)}));
        World after = w;
        apply(after, p);
        assert(plan(after, base).empty());

        // The conflict beside it is a memory cue nobody recorded the
        // origin of (an empty pad of unknown origin is no conflict now:
        // engineCuesOverEmptyPads), Engine's pad 2 its translation.
        const auto unknown = baselineOf(cuePair({hot(1, 10000), mem(20000), hot(3, 67751)}, {}));
        const auto k = plan(w, unknown);
        assert(k.cuesToEngine.empty() && k.cuesToRekordbox.size() == 1 && k.conflicts.size() == 1);
        assert(k.cuesToRekordbox[0].header.reason == EngineUpdateReason::StartCueOverEngine);
        const auto &conflict = k.conflicts[0];
        assert(conflict.header.reason == EngineUpdateReason::OriginUnknown);
        assert(conflict.header.key == "cue:memory:20000:music/a.mp3");
        assert((conflict.header.dependsOn == Deps{"cue:hot:3:music/a.mp3"}));
        assert(sameCueList(std::get<CueEdit>(conflict.engineChoice[0]).plan.cuesToApply,
                           {hot(1, 10000), hot(3, 67751), mem(20000)}));
        assert(sameCueList(std::get<CueEdit>(conflict.rekordboxChoice[0]).plan.cuesToApply,
                           {hot(1, 10000), hot(3, 67751)}));
        World settled = w;
        apply(settled, k, true);
        assert(plan(settled, unknown).empty());
        std::cout << "0:00 pad rides with a restore and beside a conflict OK\n";
    }

    // No baseline: the fallback is SyncPlanner's plan, which knows the
    // rule, said in the same words.
    {
        World w = cuePair({hot(1, 10000), hot(3, 0)}, {hot(1, 10000), hot(3, 67751)});
        const SyncPlan sync = SyncPlanner::plan(SyncMatch{w.rekordbox[0], w.engine[0]}, {}, {});
        assert(!sync.needsChoice && sync.direction == SyncPlan::Direction::ToA);
        const auto p = plan(w, std::nullopt);
        assert(p.conflicts.empty() && p.cuesToEngine.empty() && p.cuesToRekordbox.size() == 1);
        const auto &c = p.cuesToRekordbox[0];
        assert(c.header.checkedByDefault && c.header.reason == EngineUpdateReason::StartCueOverEngine);
        assert(c.header.reasonText == sync.reasonText);
        assert(c.header.reasonText
               == "rekordbox's export put a cue at 0:00 on pad 3 over the cue Engine has at 1:07.751; Engine's goes "
                  "back");
        assert((itemKeys(c.header) == Deps{"cue:hot:1:music/a.mp3", "cue:hot:3:music/a.mp3"}));
        assert(sameCueList(c.plan.cuesToApply, sync.cuesToApply));
        assert(sameCueList(c.plan.cuesToApply, {hot(1, 10000), hot(3, 67751)}));
        // With a baseline the write is the same.
        World recorded = cuePair({hot(1, 10000), hot(3, 67751)}, {hot(1, 10000), hot(3, 67751)});
        const auto base = baselineOf(recorded);
        assert(sameCueList(plan(w, base).cuesToRekordbox[0].plan.cuesToApply, c.plan.cuesToApply));
        World after = w;
        apply(after, p);
        assert(plan(after, std::nullopt).empty());
        std::cout << "no baseline: the 0:00 pad as Sync Cue Points decides OK\n";
    }

    // WS_NEW, 2026-10-10: "if rb has no cue and engine 1, and rb was
    // synched, engine wins!" Recorded empty, rekordbox holding nothing of
    // a kind on a pad where Engine holds one: Engine's goes onto rekordbox,
    // checked, never a conflict. Engine keeps a cue and a loop on one pad
    // (eight of each), rekordbox one thing a pad, so the page's "Pad 1:
    // rekordbox no cue, Engine 0:30.251; both sides changed it ...
    // (recorded empty)" is a pad where rekordbox holds a loop Engine holds
    // too, and Engine a cue beside it: the pad as a whole differed from
    // the record on both sides, so it was BothChanged, though rekordbox's
    // part of it is the same as Engine's.
    {
        struct Case
        {
            std::vector<CuePoint> rekordbox;
            std::vector<CuePoint> engine;
            std::string text;
        };
        const std::vector<Case> cases = {
            {{}, {hot(1, 30251)},
             "rekordbox has nothing on pad 1; Engine's cue at 0:30.251 goes back"},
            {{}, {hotLoop(1, 30251, 32251)},
             "rekordbox has nothing on pad 1; Engine's loop at 0:30.251 goes back"},
            {{hotLoop(1, 30251, 32251)},
             {hot(1, 30251), hotLoop(1, 30251, 32251)},
             "rekordbox has no cue on pad 1; Engine's cue at 0:30.251 goes back"},
            {{hot(1, 30251)},
             {hot(1, 30251), hotLoop(1, 30251, 32251)},
             "rekordbox has no loop on pad 1; Engine's loop at 0:30.251 goes back"},
        };
        for (const Case &c : cases) {
            for (const ValueOrigin origin : {ValueOrigin::Unknown, ValueOrigin::Seabass}) {
                World w = cuePair({hot(2, 50000)}, {hot(2, 50000)});
                auto base = baselineOf(w);  // pad 1 recorded empty
                setOrigin(base, origin);
                w.rekordbox[0].cues.insert(w.rekordbox[0].cues.end(), c.rekordbox.begin(), c.rekordbox.end());
                w.engine[0].cues.insert(w.engine[0].cues.end(), c.engine.begin(), c.engine.end());
                const auto p = plan(w, base);
                assert(p.conflicts.empty() && p.cuesToEngine.empty() && p.engineOwnKept.empty());
                assert(p.cuesToRekordbox.size() == 1);
                const auto &edit = p.cuesToRekordbox[0];
                assert(edit.header.key == "cue:hot:1:music/a.mp3" && edit.header.checkedByDefault);
                assert(!edit.header.conflict && edit.header.reason == EngineUpdateReason::EngineOverEmptyPad);
                assert(edit.header.reasonText == c.text);
                assert(edit.plan.direction == SyncPlan::Direction::ToA && !edit.plan.needsChoice);
                World after = w;
                apply(after, p);
                assert(sameCuesForSync(after.rekordbox[0].cues, after.engine[0].cues, 250.0));
                assert(plan(after, base).empty());
                assert(plan(after, baselineFrom(after.rekordbox, after.rekordboxPlaylists, 15132, stickRelative,
                                                lowerKey))
                           .empty());
                // The origin ledger records the write as Seabass's, as the
                // save does with every write onto rekordbox.
                auto ledgered = base;
                assert(recordSeabassWrites(ledgered, {{"music/a.mp3", edit.plan.cuesToApply, std::nullopt}}).empty());
                for (const auto &cue : ledgered.findTrack("music/a.mp3")->cues) {
                    assert(cue.origin == ValueOrigin::Seabass);
                }
                assert(plan(after, ledgered).empty());
                // Without a record: what Sync Cue Points decides, in the
                // same words.
                const SyncPlan sync = SyncPlanner::plan(SyncMatch{w.rekordbox[0], w.engine[0]}, {}, {});
                assert(!sync.needsChoice && sync.reason == SyncPlan::Reason::EngineOverEmptyPad);
                const auto f = plan(w, std::nullopt);
                assert(f.conflicts.empty() && f.cuesToEngine.empty() && f.cuesToRekordbox.size() == 1);
                assert(f.cuesToRekordbox[0].header.reason == EngineUpdateReason::EngineOverEmptyPad);
                assert(f.cuesToRekordbox[0].header.reasonText == c.text && sync.reasonText == c.text);
                assert(sameCueList(f.cuesToRekordbox[0].plan.cuesToApply, sync.cuesToApply));
                assert(sameCuesForSync(f.cuesToRekordbox[0].plan.cuesToApply, edit.plan.cuesToApply, 250.0));
            }
        }

        // The one exception: the record holds pad 1 as rekordbox's own,
        // and rekordbox no longer has it. The DJ deleted it in rekordbox:
        // it comes off Engine, checked, as before.
        {
            World x = cuePair({hot(1, 30251), hot(2, 50000)}, {hot(1, 30251), hot(2, 50000)});
            auto xBase = baselineOf(x);
            setOrigin(xBase, ValueOrigin::Rekordbox);
            x.rekordbox[0].cues = {hot(2, 50000)};
            const auto q = plan(x, xBase);
            assert(q.conflicts.empty() && q.cuesToRekordbox.empty() && q.cuesToEngine.size() == 1);
            assert(q.cuesToEngine[0].header.key == "cue:hot:1:music/a.mp3" && q.cuesToEngine[0].header.checkedByDefault);
            assert(q.cuesToEngine[0].header.reason == EngineUpdateReason::RekordboxRemoved);
            assert(sameCueList(q.cuesToEngine[0].plan.cuesToApply, {hot(2, 50000)}));
        }

        // A loop on rekordbox and a cue on Engine, one each on the pad:
        // two things on one pad, a conflict as before, in its words.
        World w = cuePair({hot(2, 50000)}, {hot(2, 50000)});
        const auto base = baselineOf(w);
        w.rekordbox[0].cues.push_back(hotLoop(6, 30251, 32251));
        w.engine[0].cues.push_back(hot(6, 30251));
        const auto p = plan(w, base);
        assert(p.cuesToRekordbox.empty() && p.cuesToEngine.empty() && p.conflicts.size() == 1);
        assert(p.conflicts[0].header.reason == EngineUpdateReason::BothChanged);
        assert(p.conflicts[0].header.reasonText
               == "Pad 6 is a loop on rekordbox and a cue on Engine (0:30.251); both sides changed it since Seabass "
                  "last recorded the stick (recorded empty)");
        std::cout << "an empty rekordbox pad never beats an Engine cue OK\n";
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
