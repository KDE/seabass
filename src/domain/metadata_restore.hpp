#pragma once

#include <cstdint>
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

}  // namespace seabass::domain
