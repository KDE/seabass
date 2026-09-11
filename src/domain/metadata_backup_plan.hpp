#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// One stick track and what storing it would change in the local metadata
// store.
//
// The mirror image of MetadataRestoreProposal: same three field groups,
// same merge rule, the two sides swapped. A backup reads a stick into
// the store, so here the stick is the incoming side and the store the
// existing one.
struct MetadataBackupProposal
{
    Track stickTrack;
    // The store row this would update, empty when the store has never
    // seen this track. Not an id the caller may key display on across a
    // rescan -- it is the store's own row id as text, exactly what
    // MetadataStore::readAll() puts in Track::sourceId.
    std::string storedId;
    // The store has no copy at all. Everything on the track is new, so
    // there is nothing to merge and nothing to conflict with.
    bool isNew = false;
    // How many cues the store already holds for this track, so cuesAdded()
    // can say how many of the stick's are new without the caller having
    // to carry the stored track around beside the proposal. Zero on a new
    // track, where every cue is new by definition.
    int storedCueCount = 0;
    // The stick this track's stored copy was last backed up from, for
    // display only. Empty on a new track, and empty here on every track
    // until a caller fills it in: the store keeps stick labels in a table
    // of their own (MetadataStore::stickLabelsByTrackId) rather than on
    // the domain Track, precisely so no matching or merging decision can
    // come to turn on one.
    std::string storedFrom;

    // ---- what storing would change ----------------------------------
    // Each group is decided separately by the shared merge rule, so a
    // track whose cues the stored copy wins can still hand over the
    // rating it never had.
    bool cuesOffered = false;
    bool cuesConflict = false;   // both sides have cues and they differ
    bool cuesFillAGap = false;   // the store has none and the stick does

    bool ratingOffered = false;
    bool ratingConflict = false;

    bool commentOffered = false;
    bool commentConflict = false;

    // ---- the rest of what a backup keeps current ---------------------
    // MetadataStore::store() does more than the three groups above, and
    // a plan that ignored the difference quietly stopped the store being
    // brought up to date at all: a track whose only change was a new
    // playlist counted as "already current", was never listed, was never
    // staged, and so was never handed to store(). Before this page
    // staged anything the whole collapsed list went in on every run, and
    // all of this stayed fresh for free.
    bool playCountOffered = false;
    // Membership is a set, not a conflict: store() replaces it whenever
    // the incoming reading has any, because the freshest reading is the
    // right one and there is no case where a stale membership is worth
    // keeping.
    bool playlistsOffered = false;
    // Only when the store has no cover at all. A cover is not authored
    // data, so a stored one is as good as an incoming one, and store()
    // deliberately does not re-hash images it already has.
    bool artworkOffered = false;
    // Title, artist, filename, length, bpm, key: what the software
    // wrote rather than what the DJ authored, which store() always
    // refreshes from the freshest reading. Offered when one of them
    // would actually change, so a title the DJ corrected on the stick
    // reaches the store that has to find the track by it later.
    //
    // Deliberately NOT the row's stick label or relative path, which
    // store() also refreshes. Those are bookkeeping -- where this copy
    // was last seen -- and they differ for every single track the moment
    // you back up from a different stick than last time, which would put
    // the whole library on a list whose entire purpose is to be short.
    bool identityRefresh = false;

    // A track the store would keep as it is: every field either matches
    // or lost to the stored copy. Kept out of the list the page shows,
    // because a backup that would do nothing to it is not a decision
    // anyone needs to make -- but counted, so the page can say how many
    // were already current rather than leaving them unexplained.
    bool offersAnything() const
    {
        return isNew || cuesOffered || ratingOffered || commentOffered || playCountOffered || playlistsOffered
            || artworkOffered || identityRefresh;
    }

    // How many cues storing would add, for a sentence a person reads.
    // Zero when the stored set wins, and zero rather than negative when
    // the stick's set is the smaller one -- "replaces" is the word for
    // that case and the page says it separately.
    int cuesAdded() const;
};

// What a whole planned run would do, so a page can describe it without
// walking the proposals itself.
struct MetadataBackupPlan
{
    std::vector<MetadataBackupProposal> proposals;  // only those that offer something
    int tracksSeen = 0;
    // Matched, and the store already holds everything they carry.
    int alreadyCurrent = 0;
    // No artist and title, and no filename either: nothing to recognise
    // the row by later. Not an error, and counted separately so the
    // totals add up.
    int withoutIdentity = 0;
};

// Pairs each stick track with its stored copy and works out what storing
// it would change.
//
// Matching is domain::matchTracks() unchanged, the same rule used
// everywhere else in Seabass. Neither side carries a filePath the other
// could match -- MetadataStore::readAll() leaves it empty on purpose --
// so the path branch never fires and never should: the store outlives
// the stick it was filled from, and a path does not survive a re-export
// while artist and title travel with the recording.
//
// `stickModifiedAt` is when this stick's catalogs were last written, in
// seconds since the epoch (0 if unknown); the stored side brings its own
// Track::metadataModifiedAt. An unknown time never beats a known one.
//
// Deliberately does not decide what to store. It says what would change
// and lets the caller choose which of those to hand to
// MetadataStore::store(), which stays the authority: it applies the same
// merge rule again, for real, against the row it resolves itself.
//
// Which means this is a close preview rather than a promise, and the gap
// is the matching, not the merging. The merge decision is shared code
// (domain/metadata_merge.hpp), so plan and write cannot disagree about
// whose copy wins. The row each side pairs a stick track with is found
// two different ways: here by matchTracks(), in the store by its own
// two-key SQL lookup. They agree on everything a real library has thrown
// at them so far, and where they ever differ the store is right and the
// cost is a row shown as "differs" that the run then reports as already
// current. The restore direction has the same seam, for the same reason.
MetadataBackupPlan planMetadataBackup(const std::vector<Track> &stickTracks,
                                       const std::vector<Track> &storedTracks,
                                       std::int64_t stickModifiedAt);

}  // namespace seabass::domain
