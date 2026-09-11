#include "domain/metadata_restore.hpp"

#include "domain/metadata_merge.hpp"
#include "domain/track_matching.hpp"

namespace seabass::domain
{

int MetadataRestoreProposal::cuesAdded() const
{
    if (!cuesOffered) {
        return 0;
    }
    const int after = static_cast<int>(cues.size());
    const int before = static_cast<int>(stickTrack.cues.size());
    return after > before ? after - before : 0;
}

std::vector<MetadataRestoreProposal> planMetadataRestore(const std::vector<Track> &stickTracks,
                                                          const std::vector<Track> &storedTracks,
                                                          std::int64_t stickModifiedAt)
{
    std::vector<MetadataRestoreProposal> proposals;

    for (const auto &[stick, stored] : matchTracks(stickTracks, storedTracks)) {
        MetadataRestoreProposal proposal;
        proposal.stickTrack = *stick;
        proposal.storedId = stored->sourceId;
        proposal.artworkPath = stored->artworkPath;

        // The store is the incoming side here and the stick the existing
        // one, the mirror image of what MetadataStore::store() does with
        // the same three functions. One rule, applied from both ends.
        const std::int64_t storedAt = stored->metadataModifiedAt;

        // ---- cues ----
        proposal.cuesFillAGap = stick->cues.empty() && !stored->cues.empty();
        proposal.cuesConflict =
            !stick->cues.empty() && !stored->cues.empty() && !cueSetsEqual(stick->cues, stored->cues);
        if (takeIncomingCues(stored->cues, stick->cues, storedAt, stickModifiedAt)) {
            proposal.cuesOffered = true;
            proposal.cues = stored->cues;
        }

        // ---- rating ----
        proposal.ratingConflict = stick->rating && stored->rating && *stick->rating != *stored->rating;
        if (takeIncomingRating(stored->rating, stick->rating, storedAt, stickModifiedAt)) {
            proposal.ratingOffered = true;
            proposal.rating = stored->rating;
        }

        // ---- comment ----
        proposal.commentConflict =
            !stick->comment.empty() && !stored->comment.empty() && stick->comment != stored->comment;
        if (takeIncomingComment(stored->comment, stick->comment, storedAt, stickModifiedAt)) {
            proposal.commentOffered = true;
            proposal.comment = stored->comment;
        }

        if (proposal.offersAnything()) {
            proposals.push_back(std::move(proposal));
        }
    }
    return proposals;
}

}  // namespace seabass::domain
