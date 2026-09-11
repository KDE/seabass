#include "domain/metadata_backup_plan.hpp"

#include <unordered_map>

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

        if (proposal.offersAnything()) {
            plan.proposals.push_back(std::move(proposal));
        } else {
            plan.alreadyCurrent++;
        }
    }

    return plan;
}

}  // namespace seabass::domain
