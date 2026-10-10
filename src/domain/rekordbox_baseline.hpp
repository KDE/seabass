// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
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

// RekordboxBaseline's index of its tracks by pathKey: every row of a key,
// in reader order. Built on the first lookup and again whenever the rows
// it was built over are no longer the rows there: another buffer or
// another count of rows, or a hit whose row no longer carries its key.
// What that cannot see, a row's pathKey changed in place or rows replaced
// without their count changing, the code that does it says with
// invalidate(). A copy or a move starts with no index of its own.
// Lookups are safe from several threads at once.
class BaselineTrackIndex
{
public:
    BaselineTrackIndex() = default;
    BaselineTrackIndex(const BaselineTrackIndex &) noexcept {}
    BaselineTrackIndex(BaselineTrackIndex &&) noexcept {}
    BaselineTrackIndex &operator=(const BaselineTrackIndex &) noexcept
    {
        invalidate();
        return *this;
    }
    BaselineTrackIndex &operator=(BaselineTrackIndex &&) noexcept
    {
        invalidate();
        return *this;
    }

    // The indexes into `tracks` of every row whose pathKey is `pathKey`,
    // in order; empty when none. Valid until `tracks` changes.
    const std::vector<std::size_t> &rowsOf(const std::vector<BaselineTrack> &tracks,
                                           const std::string &pathKey) const;
    void invalidate() const noexcept;

private:
    void buildLocked(const std::vector<BaselineTrack> &tracks) const;

    mutable std::mutex m_mutex;
    mutable bool m_built = false;
    mutable const BaselineTrack *m_data = nullptr;
    mutable std::size_t m_size = 0;
    mutable std::unordered_map<std::string, std::vector<std::size_t>> m_rows;
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

    // The tracks by pathKey (BaselineTrackIndex): changing a row's
    // pathKey in place, or replacing rows without changing their count,
    // is followed by trackIndex.invalidate().
    BaselineTrackIndex trackIndex;

    // nullptr when absent. Two rows for one file are both kept in tracks,
    // in reader order, and findTrack gives the first: the planner has to
    // see the duplicate, not have it folded away. findTrack goes through
    // trackIndex; the playlist lookups are linear.
    const BaselineTrack *findTrack(const std::string &pathKey) const;
    // The indexes into tracks of every row of `pathKey`, in order.
    const std::vector<std::size_t> &trackRowsOf(const std::string &pathKey) const;
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

// One entry of a playlist as a reader gives it: the track's key, its
// PlaylistMembership::position (-1 when the reader could not say) and
// the order it was met in.
struct MemberEntry
{
    int position = -1;
    std::size_t seen = 0;
    std::string pathKey;
};

// The pathKeys of `entries` in member order, baselineFrom's rule and the
// planner's and the summary's with it: positioned entries by position,
// unknown positions (-1) after them, ties in the order they were met.
std::vector<std::string> orderedMembers(std::vector<MemberEntry> entries);

// The first occurrence of each key, in order. A track listed twice in one
// playlist is planned by its first entry.
std::vector<std::string> firstOccurrences(const std::vector<std::string> &keys);

// A rating as the merge compares it: unrated and zero stars are one thing
// at the storage level (track.hpp), so 0 reads as nullopt.
std::optional<int> effectiveRating(std::optional<int> rating);

// The name of one item of the three-way merge, the key of
// RekordboxBaseline::declined and of the proposal's rows.
//
// Grammar, fields separated by ':', the pathKey always last so a ':' in
// it needs no escaping (the file layer escapes tabs and newlines):
//
//   track:<pathKey>                    the track's presence
//   playlist:<id>                      a playlist's existence and path
//   member:<playlistId>:<pathKey>      one track's membership of a playlist
//   order:<playlistId>                 the order of a playlist's members
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
    enum class Kind { Track, Playlist, Member, Order, Rating, Comment, HotCue, MemoryCue, MemoryLoop };

    Kind kind = Kind::Track;
    std::string pathKey;          // all but Playlist and Order
    std::uint32_t playlistId = 0; // Playlist, Member, Order
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
std::string orderItemKey(std::uint32_t playlistId);
std::string ratingItemKey(const std::string &pathKey);
std::string commentItemKey(const std::string &pathKey);
// Hot cue by pad, memory cue or loop by rounded position.
std::string cueItemKey(const std::string &pathKey, const CuePoint &cue);

// One write Seabass made onto the rekordbox side of a stick (a sync
// from Engine, a restore, the cue editor), keyed for the baseline.
struct SeabassWrite
{
    std::string pathKey;
    // The track's whole cue set as written; nullopt when no cues were
    // written, which is not the same as writing none.
    std::optional<std::vector<CuePoint>> cues;
    // The stars written, 0 to 5; nullopt when no rating was.
    std::optional<int> rating;
};

// The origin ledger: each written track's cues become the written set,
// every cue of Seabass origin, and its rating the written one (0 stars
// recorded as unrated, as the readers give it), of Seabass origin. Every
// row of the track's pathKey is updated. The written values are what the
// rekordbox side holds for those items now, so they are a valid base for
// them whether or not the baseline is current. Returns the pathKeys of
// writes for a track the baseline does not list, which are left out: a
// track row needs more than a write knows.
std::vector<std::string> recordSeabassWrites(RekordboxBaseline &baseline, const std::vector<SeabassWrite> &writes);

// Puts back, in `next`, the value `previous` had for each item in `keys`
// (itemKey grammar; anything else is ignored), so the item comes up again
// next time as it did this time. No previous baseline, or one without the
// item, means the item was absent from it and is taken out of `next`. Per
// kind:
//
//   track       presence only: a row previous had and next lacks is put
//               back whole, a row next has and previous lacked is taken
//               out; a row both have is left as next has it
//   playlist    path, parent and folder flag; put back with no members
//               (each member is an item of its own) or taken out
//   member      the track's entries in that playlist: next's taken out,
//               previous's put back at their indexes (clamped)
//   order       the members previous has, in the places they take in
//               next, put back in previous's order (first entries of a
//               track listed twice); members come and go only as
//               items of their own, and a playlist previous lacks keeps
//               next's order
//   rating, comment, cue
//               the value on every row of the track next has; a cue is
//               every cue of that key, with its origin
//
// Tracks and playlists are settled before members, members before
// orders, and orders before values, whatever the order of `keys`.
void keepPreviousItems(RekordboxBaseline &next, const RekordboxBaseline *previous, const std::set<std::string> &keys);

// The baseline a save of Sync after Rekordbox Export records (plan, "When
// the baseline is written", case 1): every item advances to rekordbox as
// it is now (`rekordboxNow`, `playlists`, `sequence`, through
// baselineFrom and the same two path functions), except the items the
// proposal `offered` that the save did not apply (`applied`) and every
// `declined` one, which keep their value from `previous`
// (keepPreviousItems). Already-level items are in neither list and
// advance.
//
// `declined` maps itemKey to rekordbox's state hash when declined, and
// becomes the x entries; an x entry of `previous` for an item kept here
// and not declined again is carried, so an item the planner suppressed
// as declined stays declined. A cue or rating that advances with the
// value `previous` had keeps `previous`'s origin: the value did not
// change, so neither did who put it there. engineUuid is `previous`'s;
// recordedAtUnix and writer are left for the writer to fill.
//
// Seabass's own writes of the same save (restores onto rekordbox) are not
// known here; the save merges them afterwards (recordSeabassWrites).
RekordboxBaseline nextBaseline(const RekordboxBaseline *previous, const std::vector<Track> &rekordboxNow,
                               const std::vector<PlaylistInfo> &playlists, std::uint64_t sequence,
                               const std::set<std::string> &offered, const std::set<std::string> &applied,
                               const std::map<std::string, std::string> &declined,
                               const std::function<std::string(const std::string &)> &stickRelativeOf,
                               const std::function<std::string(const std::string &)> &pathKeyOf,
                               BaselineGaps *gaps = nullptr);

}  // namespace seabass::domain
