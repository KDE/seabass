// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// How the rekordbox side of a stick looked at the last point Seabass knew
// it level with Engine: the B of Sync after Rekordbox Export's three-way
// merge (docs/sync-after-rekordbox-export-plan.md).
//
// Neither catalog records who changed what. export.pdb's sequence moves
// on every write, Engine's pdbImportKey says only "came from an import",
// and no clock decides a sync. So Seabass keeps its own record and plans
// per item: where R differs from B and E still equals B, rekordbox
// changed it. Types and baselineFrom only; reading and writing the file
// is infrastructure's.

// Who put a cue or a rating on the rekordbox side. Seabass knows its own
// writes (a sync from Engine, the cue editor); a rekordbox export
// regenerates the analysis files from master.db and drops them. Rekordbox
// means the export wrote it; Unknown is everything nobody recorded,
// including all of a baseline built from a read, and the planner turns
// Unknown into a conflict rather than a guess.
enum class ValueOrigin { Rekordbox, Seabass, Unknown };

struct BaselineCue
{
    CuePoint cue;
    ValueOrigin origin = ValueOrigin::Unknown;
};

// One export.pdb track. pathKey is what the three sides are paired on:
// the stick-relative path through the caller's normalizedPathKey, so it
// survives the stick mounting somewhere else.
struct BaselineTrack
{
    std::string pathKey;
    std::string stickRelativePath;
    // export.pdb's analyze_path as the catalog spells it: where the
    // player takes the cues from, and what a restore writes.
    std::string analysisFile;
    std::uint32_t pdbId = 0;  // 0 when the row's sourceId is not a number
    std::optional<int> rating;  // 0 to 5 stars, nullopt when unrated
    ValueOrigin ratingOrigin = ValueOrigin::Unknown;
    std::string comment;
    double bpm = 0.0;
    std::int64_t durationMs = 0;
    std::vector<BaselineCue> cues;
};

// One export.pdb playlist or folder. The id, not the path, says which
// playlist it is: a rename keeps the id and changes the path, and an
// empty playlist is here although no track names it.
struct BaselinePlaylist
{
    std::uint32_t id = 0;
    std::uint32_t parentId = 0;  // 0 at the top level, as export.pdb has it
    bool folder = false;
    std::string path;  // "Folder/List", spelled as PlaylistMembership::name
    // Ordered member pathKeys; a track listed twice is two entries.
    // Always empty for a folder.
    std::vector<std::string> members;
};

struct RekordboxBaseline
{
    // export.pdb's header sequence when this was recorded. A baseline
    // whose sequence equals the stick's is current: rekordbox has not
    // exported since.
    std::uint64_t pdbSequence = 0;
    // Engine's Information.uuid, so a baseline is not read against a
    // different Engine library on the same stick. Filled by the writer.
    std::string engineUuid;
    std::int64_t recordedAtUnix = 0;  // for the reader of the file; never decides anything
    std::string writer;               // the Seabass version that wrote it, same
    std::vector<BaselineTrack> tracks;
    std::vector<BaselinePlaylist> playlists;
    // Items the user declined, by itemKey, each with a hash of rekordbox's
    // state of that item when it was declined. The page stays quiet about
    // the item until rekordbox changes it again and the hash differs.
    std::map<std::string, std::string> declined;

    // Linear lookups; nullptr when absent. Two rows for one file are both
    // kept in tracks, in reader order, and findTrack gives the first:
    // the planner has to see the duplicate, not have it folded away.
    const BaselineTrack *findTrack(const std::string &pathKey) const;
    const BaselinePlaylist *findPlaylist(std::uint32_t id) const;
    const BaselinePlaylist *findPlaylistByPath(const std::string &path) const;
};

// One export.pdb playlist tree row, as a reader gives it: what
// Track::playlists cannot say (folders, ids, empty playlists).
struct PlaylistInfo
{
    std::string path;  // "Folder/List"
    bool folder = false;
    std::uint32_t id = 0;
};

// What baselineFrom could not place, so a caller can say so rather than
// record a baseline that silently lacks it.
struct BaselineGaps
{
    // sourceIds of rows with no path to key on (pathKeyOf gave "").
    std::vector<std::string> unkeyedTracks;
    // Membership names that match no playlist in `playlists`, or name a
    // folder, as "<sourceId> in <name>". Dropped from every member list.
    std::vector<std::string> unknownMemberships;
    // Playlist paths whose parent path is in no PlaylistInfo. Recorded at
    // the top level (parentId 0).
    std::vector<std::string> orphanPlaylists;
};

// The baseline the readers' view of the rekordbox side gives, every cue
// and rating of Unknown origin: a read cannot tell Seabass's writes from
// the export's, the origin ledger of the saves can (plan step 9).
//
// `stickRelativeOf` turns a Track::filePath (absolute, on whatever mount
// point the stick has today) into the path from the stick root, and
// `pathKeyOf` turns that into the comparison key (the application's
// normalizedPathKey). Both are injected because the domain does not know
// the filesystem or the application layer. A row whose key comes out
// empty is left out and listed in `gaps`.
//
// Members are ordered by PlaylistMembership::position. Entries whose
// position the reader could not determine (-1) follow the positioned
// ones, in the order of `rekordbox`, and ties keep that order too. A
// membership with a playlistId goes to the playlist of that id, one
// without to the first playlist of its path. The parent of "A/B/C" is
// the first playlist whose path is "A/B".
RekordboxBaseline baselineFrom(const std::vector<Track> &rekordbox, const std::vector<PlaylistInfo> &playlists,
                               std::uint64_t pdbSequence,
                               const std::function<std::string(const std::string &)> &stickRelativeOf,
                               const std::function<std::string(const std::string &)> &pathKeyOf,
                               BaselineGaps *gaps = nullptr);

// The name of one item of the three-way merge, the key of
// RekordboxBaseline::declined and of the proposal's rows.
//
// Grammar, fields separated by ':', the pathKey always last so a ':' in
// it needs no escaping (the file layer escapes tabs and newlines):
//
//   track:<pathKey>                    the track's presence
//   playlist:<id>                      a playlist's existence and path
//   member:<playlistId>:<pathKey>      one track's membership of a playlist
//   rating:<pathKey>
//   comment:<pathKey>
//   cue:hot:<pad>:<pathKey>            hot cue or hot loop on pad <pad>, from 1
//   cue:memory:<ms>:<pathKey>          memory cue at <ms>, rounded
//   cue:loop:<ms>:<pathKey>            memory loop starting at <ms>, rounded
//
// Ids, pads and milliseconds are plain decimal. A memory cue is named by
// its position because it has no other identity; matching within a
// tolerance is the planner's business, which names the item by the
// baseline's position.
struct ItemKey
{
    enum class Kind { Track, Playlist, Member, Rating, Comment, HotCue, MemoryCue, MemoryLoop };

    Kind kind = Kind::Track;
    std::string pathKey;          // all but Playlist
    std::uint32_t playlistId = 0; // Playlist, Member
    int pad = 0;                  // HotCue
    std::int64_t positionMs = 0;  // MemoryCue, MemoryLoop

    bool operator==(const ItemKey &) const = default;
};

std::string itemKey(const ItemKey &key);
// nullopt for anything the grammar above does not produce, including a
// number with trailing garbage or an empty pathKey.
std::optional<ItemKey> parseItemKey(const std::string &text);

std::string trackItemKey(const std::string &pathKey);
std::string playlistItemKey(std::uint32_t id);
std::string memberItemKey(std::uint32_t playlistId, const std::string &pathKey);
std::string ratingItemKey(const std::string &pathKey);
std::string commentItemKey(const std::string &pathKey);
// Hot cue by pad, memory cue or loop by rounded position.
std::string cueItemKey(const std::string &pathKey, const CuePoint &cue);

}  // namespace seabass::domain
