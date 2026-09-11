#include "domain/metadata_backup_plan.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "domain/metadata_merge.hpp"
#include "domain/track_matching.hpp"

namespace seabass::domain
{

namespace
{

// The store's own rule for "is there anything to recognise this row by
// later", mirrored so a track the write would skip is not offered as a
// decision here. See MetadataStore::store(): a streaming link names no
// file on any stick, and a row with neither title+artist nor a filename
// has no key anything could match it with afterwards.
bool hasStorableIdentity(const Track &track)
{
    if (!track.streamingSource.empty()) {
        return false;
    }
    return titleArtistKey(track).has_value() || !normalizeFilename(track.filename).empty();
}

// Membership as a set of names. Order is not meaningful -- two catalogs
// list the same playlists in whatever order they walk them -- so this
// sorts before comparing rather than treating a reshuffle as a change
// and putting the whole library on the list.
bool playlistNamesEqual(const Track &a, const Track &b)
{
    std::vector<std::string> left;
    std::vector<std::string> right;
    left.reserve(a.playlists.size());
    right.reserve(b.playlists.size());
    for (const auto &member : a.playlists) {
        left.push_back(member.name);
    }
    for (const auto &member : b.playlists) {
        right.push_back(member.name);
    }
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right;
}

// Would storing this track change any of the fields store() refreshes
// from the freshest reading?
//
// Each one only counts when the incoming value is non-empty, because
// that is exactly the condition store()'s own UPDATE applies: it writes
// `CASE WHEN ? <> '' THEN ? ELSE <column> END`, so a catalog that failed
// to read a title cannot blank the stored one and must not be reported
// as though it would.
bool identityWouldChange(const Track &stick, const Track &stored)
{
    const auto textChanges = [](const std::string &incoming, const std::string &existing) {
        return !incoming.empty() && incoming != existing;
    };
    if (textChanges(stick.title, stored.title) || textChanges(stick.artist, stored.artist)
        || textChanges(stick.filename, stored.filename) || textChanges(stick.key, stored.key)) {
        return true;
    }
    // Numbers use the same "only a real reading counts" rule, with a
    // tolerance so a rounding in one catalog's reading of a length or a
    // tempo does not list every track on the stick.
    const auto numberChanges = [](double incoming, double existing, double tolerance) {
        return incoming > 0.0 && std::abs(incoming - existing) > tolerance;
    };
    return numberChanges(stick.durationSeconds, stored.durationSeconds, 2.0)
        || numberChanges(stick.bpm, stored.bpm, 0.05);
}

}  // namespace

int MetadataBackupProposal::cuesAdded() const
{
    if (!cuesOffered) {
        return 0;
    }
    const int after = static_cast<int>(stickTrack.cues.size());
    // A new track is measured against nothing, so every cue on it is
    // added. An existing one is measured against what the store already
    // holds. Never negative: when the stick's set is the smaller one the
    // word is "replaces", and the page says that separately.
    const int before = isNew ? 0 : storedCueCount;
    return after > before ? after - before : 0;
}

MetadataBackupPlan planMetadataBackup(const std::vector<Track> &stickTracks,
                                       const std::vector<Track> &storedTracks,
                                       std::int64_t stickModifiedAt)
{
    MetadataBackupPlan plan;

    // Walked in stick order below rather than in match order, so the
    // list a page shows is the stick's own running order with the new
    // tracks sitting where they actually are, instead of every new track
    // bunched at one end by an accident of how matching returns pairs.
    std::unordered_map<const Track *, const Track *> storedFor;
    const auto matched = matchTracks(stickTracks, storedTracks);
    storedFor.reserve(matched.size());
    for (const auto &[stick, stored] : matched) {
        storedFor.emplace(stick, stored);
    }

    for (const Track &stick : stickTracks) {
        plan.tracksSeen++;
        if (!hasStorableIdentity(stick)) {
            plan.withoutIdentity++;
            continue;
        }

        MetadataBackupProposal proposal;
        proposal.stickTrack = stick;

        const auto found = storedFor.find(&stick);
        if (found == storedFor.end()) {
            // Nothing to merge against: every field it carries is the
            // only copy there is. Offered as a whole rather than field
            // by field, because "the store has never seen this track" is
            // one fact and not three.
            proposal.isNew = true;
            proposal.cuesOffered = !stick.cues.empty();
            proposal.cuesFillAGap = proposal.cuesOffered;
            proposal.ratingOffered = stick.rating.has_value();
            proposal.commentOffered = !stick.comment.empty();
            proposal.playCountOffered = stick.playCount.has_value();
            proposal.playlistsOffered = !stick.playlists.empty();
            proposal.artworkOffered = !stick.artworkPath.empty();
            plan.proposals.push_back(std::move(proposal));
            continue;
        }

        const Track *stored = found->second;
        proposal.storedId = stored->sourceId;
        proposal.storedCueCount = static_cast<int>(stored->cues.size());

        // The stick is the incoming side here and the store the existing
        // one -- the mirror image of planMetadataRestore, which runs the
        // same three functions the other way round. One rule, applied
        // from both ends.
        const std::int64_t storedAt = stored->metadataModifiedAt;

        // ---- cues ----
        proposal.cuesFillAGap = stored->cues.empty() && !stick.cues.empty();
        proposal.cuesConflict =
            !stored->cues.empty() && !stick.cues.empty() && !cueSetsEqual(stored->cues, stick.cues);
        proposal.cuesOffered = takeIncomingCues(stick.cues, stored->cues, stickModifiedAt, storedAt);

        // ---- rating ----
        proposal.ratingConflict = stick.rating && stored->rating && *stick.rating != *stored->rating;
        proposal.ratingOffered = takeIncomingRating(stick.rating, stored->rating, stickModifiedAt, storedAt);

        // ---- comment ----
        proposal.commentConflict =
            !stick.comment.empty() && !stored->comment.empty() && stick.comment != stored->comment;
        proposal.commentOffered = takeIncomingComment(stick.comment, stored->comment, stickModifiedAt, storedAt);

        // ---- play count ----
        proposal.playCountOffered =
            takeIncomingPlayCount(stick.playCount, stored->playCount, stickModifiedAt, storedAt);

        // ---- playlist membership ----
        // Compared as a set of names, which is what store() writes and
        // all the store shows. Position within a playlist rides along
        // and is not worth listing a track for on its own.
        proposal.playlistsOffered = !stick.playlists.empty() && !playlistNamesEqual(stick, *stored);

        // ---- cover art ----
        // Only into a gap, mirroring store(), which does not re-hash an
        // image it already has.
        proposal.artworkOffered = stored->artworkPath.empty() && !stick.artworkPath.empty();

        // ---- the fields the software wrote ----
        proposal.identityRefresh = identityWouldChange(stick, *stored);

        if (proposal.offersAnything()) {
            plan.proposals.push_back(std::move(proposal));
        } else {
            plan.alreadyCurrent++;
        }
    }

    return plan;
}

}  // namespace seabass::domain
