// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// One stick track and what the store has to offer it.
//
// Each field group is decided separately, so a track whose cues conflict
// can still be given the comment it never had.
struct MetadataRestoreProposal
{
    Track stickTrack;
    std::string storedId;   // the store row this came from
    std::string storedFrom; // the stick label the store last saw it on, for display
    // And that stick's identity, when the store recorded one. What the
    // restore page's stick picker keys on: two sticks called NO NAME is
    // the ordinary case, and a label alone would fold them into one.
    // Filled by the caller, like storedFrom, because the store keeps
    // where a row was last seen out of domain::Track on purpose.
    std::string storedFromLibraryId;
    // The playlists the backup recorded this track in, from the stored
    // side. With the stick track's own playlists, what the restore
    // page's playlist picker narrows on (see proposalInRestoreScope).
    std::vector<std::string> storedPlaylists;
    // The cover the store copied for this track, absolute, on this
    // computer. From the stored side rather than the stick's, because
    // this row is showing what the store is offering -- and because the
    // case the whole page exists for is a stick that has lost the lot.
    std::string artworkPath;

    // The complete cue list to write, never a diff: every cue writer in
    // Seabass replaces a track's whole set, so anything less silently
    // drops what is already there.
    std::vector<CuePoint> cues;
    bool cuesOffered = false;
    // The stick already has cues and they are not the stored ones.
    // Whether the offer was made anyway is cuesOffered: the merge rule
    // decides, and a conflict the stored side won is both a conflict and
    // an offer. Kept apart so the row can say "replaces 4" rather than
    // presenting a replacement as a gap being filled.
    bool cuesConflict = false;
    // The headline case: the stick has no cues at all for this track and
    // the store has some. Unambiguous, and what a re-export leaves
    // behind.
    bool cuesFillAGap = false;

    std::optional<int> rating;
    bool ratingOffered = false;
    bool ratingConflict = false;

    std::string comment;
    bool commentOffered = false;
    bool commentConflict = false;

    bool offersAnything() const { return cuesOffered || ratingOffered || commentOffered; }
    // How many cues the write would add, for a sentence a person reads.
    int cuesAdded() const;
};

// Pairs each stick track with its stored copy and works out what a
// restore would put back.
//
// Matching is domain::matchTracks() unchanged: artist and title, falling
// back to filename, guarded by length. Neither side carries a filePath
// the other could match -- MetadataStore::readAll leaves it empty on
// purpose -- so the path branch never fires and never should. The store
// outlives the stick it was filled from, which is exactly the case a
// path cannot survive: a re-export renames folders and a track bought
// again lands somewhere else entirely, while artist and title travel
// with the recording.
//
// What is offered is decided per field by the shared merge rule
// (domain/metadata_merge.hpp), the same one the backup direction uses,
// with the two sides swapped. `stickModifiedAt` is when this stick's
// catalogs were last written, in seconds since the epoch (0 if unknown);
// the stored side brings its own Track::metadataModifiedAt.
//
// Returns only proposals that offer something. A track already carrying
// everything the store has is not a decision anyone needs to make.
std::vector<MetadataRestoreProposal> planMetadataRestore(const std::vector<Track> &stickTracks,
                                                          const std::vector<Track> &storedTracks,
                                                          std::int64_t stickModifiedAt);

// ---- narrowing a restore to one stick's backup, or one playlist ------
//
// The restore page offers the same two pickers the backup page has: which
// stick's backup to restore from, and which playlist. They narrow what a
// restore WRITES, not only what the list shows: Select All stages what
// is in scope, and changing the scope unstages what falls outside it, so
// the save writes exactly the in-scope tracks that are ticked. These are
// the functions that decide "in scope", in one place, so the page, the
// counts and the staging cannot disagree about it.

// A stick the backup came from, as the picker keys it: the library id
// the store recorded, or the label when it recorded none. Prefixed so a
// label can never collide with an id. Never empty, because an empty
// key is MetadataRestoreScope's "every stick": a row the store knows
// neither for is keyed "unknown", which no "id:" or "label:" key can
// spell, and is a stick of its own in the picker. Keyed empty, picking
// it selected every stick, and the picker showed it while every stick
// was in scope.
std::string restoreSourceKey(const MetadataRestoreProposal &proposal);

// Every playlist a proposal is in: the backup's record of it and the
// stick's own, once each. Both, because a restore goes to whichever
// stick is in front of you, whose playlists may have been rebuilt since
// the backup, and a DJ asking for "Warm Up" means the playlist of that
// name wherever it is recorded.
std::vector<std::string> restorePlaylistsOf(const MetadataRestoreProposal &proposal);

struct MetadataRestoreScope
{
    std::string sourceKey;  // restoreSourceKey() of one stick; empty = every stick
    std::string playlist;   // one playlist name; empty = every track
};

bool proposalInRestoreScope(const MetadataRestoreProposal &proposal, const MetadataRestoreScope &scope);

// One entry of the stick picker: a stick the backup holds proposals
// from, and how many. Sorted by label, then key, so the picker does not
// reorder itself between two scans of one stick.
struct MetadataRestoreSource
{
    std::string key;
    std::string label;
    int proposalCount = 0;
};
std::vector<MetadataRestoreSource> restoreSources(const std::vector<MetadataRestoreProposal> &proposals);

// Every playlist among the proposals from `sourceKey` (empty = every
// stick), with how many proposals each holds. Counted over proposals,
// not over tracks on the stick: the number beside a playlist is how many
// tracks picking it would restore.
std::map<std::string, int> restorePlaylistCounts(const std::vector<MetadataRestoreProposal> &proposals,
                                                 const std::string &sourceKey);

}  // namespace seabass::domain
