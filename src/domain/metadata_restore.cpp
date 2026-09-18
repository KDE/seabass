// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/metadata_restore.hpp"

#include "domain/junk_cue.hpp"

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
    // Against the cues that count: a stick whose three cues are all
    // strays gains every cue this writes, and saying "adds 1" because it
    // had three of something the write drops is the wrong number.
    const int before = static_cast<int>(withoutJunkMemoryCues(stickTrack.cues).size());
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
        //
        // Neither side's stray cues take part. A memory cue at 0:00 is a
        // fault (domain/junk_cue.hpp), so the store must not offer one
        // back -- older backups were taken before this was filtered on
        // the way in and still hold them -- and a stick carrying nothing
        // but strays is a stick with no cues, which is a gap to fill
        // rather than a conflict to weigh. What the restore writes is a
        // whole cue list, so a single stray left in it would be written
        // back with the rest.
        const std::vector<CuePoint> storedCues = withoutJunkMemoryCues(stored->cues);
        const std::vector<CuePoint> stickCues = withoutJunkMemoryCues(stick->cues);
        proposal.cuesFillAGap = stickCues.empty() && !storedCues.empty();
        proposal.cuesConflict =
            !stickCues.empty() && !storedCues.empty() && !cueSetsEqual(stickCues, storedCues);
        if (takeIncomingCues(storedCues, stickCues, storedAt, stickModifiedAt)) {
            proposal.cuesOffered = true;
            proposal.cues = storedCues;
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
