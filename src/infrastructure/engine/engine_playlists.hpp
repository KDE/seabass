// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "domain/engine_update_planning.hpp"

namespace seabass::infrastructure::engine
{

// Playlist entries in m.db that name a Track row that does not exist. A
// player skips them, and the Engine reader drops them (no Track row
// carries the membership), so nothing reading through it could see them:
// WHALESHARK2's "Whaleshark Cooldown" had 8 (#61). Read straight from
// SQLite, read-only.
struct DanglingPlaylistEntries
{
    std::string playlist;  // the playlist's own title (not its path)
    int entries = 0;
};
std::vector<DanglingPlaylistEntries> danglingPlaylistEntries(const std::string &engineLibraryPath);

// Deletes those entries from the m.db at `databaseFile`. Engine's own
// trigger (trigger_before_delete_PlaylistEntity) relinks each playlist's
// chain around a deleted entry, so the rest keep their order. Returns how
// many were deleted; throws when the database cannot be written.
int removeDanglingPlaylistEntries(const std::string &databaseFile);

// A playlist's membership in the library at `engineLibraryPath` (a write
// root: the stick's library or a save's scratch copy), the playlist named
// by its full path as the Engine reader spells it ("Folder/List"). Add
// appends at the end unless the track is already a member; remove takes
// the track out. Throw when the playlist or the track does not exist.
// Return whether anything changed.
//
// Remove deletes the track's PlaylistEntity rows in one transaction and
// commits only when the rest of the list still walks as one chain in its
// old order (refusing a list whose chain is already broken, or a path two
// playlists spell). It does not use libdjinterop's playlist::remove_track,
// which deletes by the entry's own id while handed the track's (the note
// in libdjinterop_engine_cleanup_writer.cpp). m.db keeps one entry per
// (list, databaseUuid, track), so a track sits in a list once per
// databaseUuid; remove takes out every one.
bool addToEnginePlaylist(const std::string &engineLibraryPath, const std::string &playlistPath, std::int64_t trackId);
bool removeFromEnginePlaylist(const std::string &engineLibraryPath, const std::string &playlistPath,
                              std::int64_t trackId);

// Whether the library at `engineLibraryPath` has a playlist at
// playlistPath ("Folder/List"). False when the library cannot be opened.
bool enginePlaylistExists(const std::string &engineLibraryPath, const std::string &playlistPath);

// The ids of the tracks whose file is `filePath` (absolute), read from the
// library at `databaseRoot` (a write root, maybe a scratch copy), by the
// path Engine stores relative to the stick's real library folder
// `realLibraryPath`.
std::vector<std::int64_t> engineTrackIdsForFile(const std::string &databaseRoot, const std::string &realLibraryPath,
                                                const std::string &filePath);

// How many playlists (or folders) spell `playlistPath` ("Folder/List"):
// more than one when two share a name in one folder, or a name holds a
// "/" that reads as a folder. Only exactly one can be edited by path.
int enginePlaylistCountAtPath(const std::string &engineLibraryPath, const std::string &playlistPath);

// Every playlist and folder in the library, depth first in Engine's
// order, each with its path as the Engine reader spells it, whether it
// has children (Engine's only notion of a folder), and how many
// playlists spell that path (enginePlaylistCountAtPath's reading, taken
// in the same walk). For Sync after Rekordbox Export's planner. Throws
// when the library cannot be opened.
std::vector<domain::EnginePlaylistInfo> listEnginePlaylists(const std::string &engineLibraryPath);

// The ids of every track some playlist in the library holds.
std::vector<std::int64_t> engineTracksInAnyPlaylist(const std::string &engineLibraryPath);

// Removes the tracks with these ids from the library (rows only; the
// files are the caller's), each with every row that names it, through
// removeEngineTrackRows (engine_track_rows.hpp) in one transaction. A
// track still in a playlist is refused with a throw before anything is
// written: Delete Tracks leaves listed tracks alone, and this keeps that
// rule for every caller; removeEngineTrackRows is the removal without it.
// An id with no track is skipped. Returns how many were removed; throws
// when the database cannot be written.
int removeEngineTracks(const std::string &engineLibraryPath, const std::vector<std::int64_t> &trackIds);

// Deletes the playlist at playlistPath ("Folder/List") with every playlist
// below it, from the library at `engineLibraryPath` (a write root). Each
// playlist's entries are cleared first: m.db only cascades them with
// foreign keys on, which nothing guarantees. Tracks stay. Returns how many
// playlists went (0 when there is no such playlist); throws when the
// database cannot be written or an entry survives.
int deleteEnginePlaylist(const std::string &engineLibraryPath, const std::string &playlistPath);

// Creating, renaming and ordered inserting, for a sync that follows
// rekordbox's playlist tree. Like the writers above they work on a write
// root (the stick's library or a save's scratch copy), name a playlist by
// the path the Engine reader spells ("Folder/List", titles joined by
// "/"), and throw std::runtime_error for every refusal, before anything
// is written, as well as when the database cannot be written or a write
// does not read back as asked.
//
// A path must name exactly one playlist to be used. m.db keeps titles
// unique within one parent (C_NAME_UNIQUE_FOR_PARENT), so two playlists
// spell one path only when a title holds a "/": a root playlist titled
// "A/B" and a list "B" in a folder "A" both read as "A/B". These functions
// count every playlist that spells a path (enginePlaylistCountAtPath's
// reading) and refuse when it is not exactly one; a write that would make
// a path ambiguous is refused the same way.
//
// How the chains stay walkable. Engine orders playlists and their entries
// as linked lists: Playlist.nextListId among the children of one parent,
// PlaylistEntity.nextEntityId within one list, 0 ending each. libdjinterop
// reads both by starting at the row whose next is 0 and following, for
// each row, the one row that points at it, back to the head
// (playlist_entity_table::get_for_list, playlist_table's sort_ids). A
// broken link therefore does not fail: everything ahead of the break is
// silently left out, and the player shows the list shortened. A list with
// no row ending in 0 trips an assert, undefined in a release build.
//
// These three write m.db with SQL, the statements libdjinterop's
// playlist_table and playlist_entity_table run, in one transaction per
// call: its public playlist handles carry no id, its table classes come
// in a 2.x and a 3.x flavour (the 2.x one will not open a 3.x library),
// and its set_parent is wrong (below).
//
// - createEnginePlaylist inserts each new playlist with nextListId 0 and
//   Engine's own triggers (trigger_before_insert_List and
//   trigger_after_insert_List) point the parent's previous last child at
//   it, as create_root_playlist and create_sub_playlist do.
// - renameEnginePlaylist within one parent changes the title only. A move
//   runs playlist_table::update's four statements with nextListId 0 (Engine
//   has no UPDATE trigger for this): the playlist is unlinked from its old
//   siblings and appended to the new parent's children. It is not
//   playlist::set_parent: that keeps the old nextListId, so a playlist
//   that was not its old parent's last child points into the old folder
//   from the new one and is missing from the new folder's children
//   (engine_playlists_test pins it).
// - insertIntoEnginePlaylist adds the entry with the anchor's old
//   nextEntityId (or 0 to append) and points the one entry that had that
//   next (the anchor, or the old last entry) at it, as add_track_after and
//   add_track_back do through playlist_entity_table::add. Entries have no
//   insert trigger; trigger_before_delete_PlaylistEntity relinks around a
//   deleted one.
//
// Each function walks the chains it touched itself afterwards, from the
// head, and throws (rolling the transaction back) when one does not reach
// every row or the result is not what was asked.

// Creates the playlist at playlistPath ("A/B/C"), and every playlist above
// it that does not exist yet, as an empty folder. In Engine a folder is
// any playlist with children, so an existing playlist with entries can
// hold the new one. Each new playlist goes last among its siblings.
// Refuses an empty path or segment, a title with ";" (Engine will not
// take one), a path that already names a playlist, a prefix that names
// more than one, and a missing prefix some playlist already spells
// further down (creating it would make that spelling ambiguous). Returns
// the new playlist's id.
std::int64_t createEnginePlaylist(const std::string &engineLibraryPath, const std::string &playlistPath);

// Renames the playlist at fromPath to toPath: a new title within the same
// parent, or a move into the existing playlist named by toPath's parent
// path (moved last among its new siblings). The playlist keeps its id,
// its entries in their order, and its own children, which move with it.
// Refuses a fromPath that does not name exactly one playlist, a toPath
// that already names one, a new parent that does not exist or is
// ambiguous (the sync creates parents first, through create rows the
// rename depends on), a move into the playlist itself or below it, and a
// rename that would make one of its children's paths ambiguous. Returns
// false when fromPath and toPath are the same, true when it renamed.
bool renameEnginePlaylist(const std::string &engineLibraryPath, const std::string &fromPath, const std::string &toPath);

// Puts the track into the playlist at playlistPath right after the entry
// for afterTrackId. With no anchor, or an anchor that is not in the list
// (the plan's rule: a writer appends when its anchor is missing), it goes
// last. A track already in the list stays where it is and false is
// returned, as addToEnginePlaylist does: m.db cannot hold a track twice in
// one list (C_NAME_UNIQUE_FOR_LIST), so there is no duplicate to ask for.
// Refuses a playlist path that does not name exactly one playlist, a
// track id with no track, and a list whose entry chain is already broken
// (an insert would hide or lose part of it). Returns true when it added.
bool insertIntoEnginePlaylist(const std::string &engineLibraryPath, const std::string &playlistPath,
                              std::int64_t trackId, std::optional<std::int64_t> afterTrackId);

// Puts the track first in the playlist at playlistPath: what a sync asks
// for when no member before it in rekordbox's playlist is in Engine's
// (MembershipEdit's empty anchor), which insertIntoEnginePlaylist cannot
// say, since it appends without an anchor. The new entry points at the
// old head and no entry points at it. Into an empty list it is the one
// entry. A member stays where it is and false is returned. Refuses as
// insertIntoEnginePlaylist does. Returns true when it added.
bool insertAtStartOfEnginePlaylist(const std::string &engineLibraryPath, const std::string &playlistPath,
                                   std::int64_t trackId);

}  // namespace seabass::infrastructure::engine
