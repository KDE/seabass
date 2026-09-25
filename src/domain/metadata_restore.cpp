// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/metadata_restore.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

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
    const int before = static_cast<int>(withoutJunkCues(stickTrack.cues).size());
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
        for (const auto &member : stored->playlists) {
            if (std::find(proposal.storedPlaylists.begin(), proposal.storedPlaylists.end(), member.name)
                == proposal.storedPlaylists.end()) {
                proposal.storedPlaylists.push_back(member.name);
            }
        }

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
        const std::vector<CuePoint> storedCues = withoutJunkCues(stored->cues);
        const std::vector<CuePoint> stickCues = withoutJunkCues(stick->cues);
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

std::string restoreSourceKey(const MetadataRestoreProposal &proposal)
{
    if (!proposal.resolvedLibraryId.empty()) {
        return "id:" + proposal.resolvedLibraryId;
    }
    if (!proposal.storedFromLibraryId.empty()) {
        return "id:" + proposal.storedFromLibraryId;
    }
    if (!proposal.storedFrom.empty()) {
        return "label:" + proposal.storedFrom;
    }
    // Not empty: that is MetadataRestoreScope's "every stick".
    return "unknown";
}

namespace
{

// Every library id the proposals recorded under each label.
std::map<std::string, std::set<std::string>> recordedIdsByLabel(const std::vector<MetadataRestoreProposal> &proposals)
{
    std::map<std::string, std::set<std::string>> ids;
    for (const auto &proposal : proposals) {
        if (!proposal.storedFrom.empty() && !proposal.storedFromLibraryId.empty()) {
            ids[proposal.storedFrom].insert(proposal.storedFromLibraryId);
        }
    }
    return ids;
}

}  // namespace

void resolveRestoreSources(std::vector<MetadataRestoreProposal> &proposals)
{
    const auto ids = recordedIdsByLabel(proposals);
    for (auto &proposal : proposals) {
        proposal.resolvedLibraryId.clear();
        if (!proposal.storedFromLibraryId.empty() || proposal.storedFrom.empty()) {
            continue;
        }
        const auto found = ids.find(proposal.storedFrom);
        if (found != ids.end() && found->second.size() == 1) {
            proposal.resolvedLibraryId = *found->second.begin();
        }
    }
}

std::vector<std::string> restorePlaylistsOf(const MetadataRestoreProposal &proposal)
{
    std::vector<std::string> names = proposal.storedPlaylists;
    for (const auto &member : proposal.stickTrack.playlists) {
        if (std::find(names.begin(), names.end(), member.name) == names.end()) {
            names.push_back(member.name);
        }
    }
    return names;
}

bool proposalInRestoreScope(const MetadataRestoreProposal &proposal, const MetadataRestoreScope &scope)
{
    if (!scope.sourceKey.empty() && restoreSourceKey(proposal) != scope.sourceKey) {
        return false;
    }
    if (!scope.playlist.empty()) {
        const auto names = restorePlaylistsOf(proposal);
        if (std::find(names.begin(), names.end(), scope.playlist) == names.end()) {
            return false;
        }
    }
    return true;
}

std::vector<MetadataRestoreSource> restoreSources(const std::vector<MetadataRestoreProposal> &proposals)
{
    const auto ids = recordedIdsByLabel(proposals);
    std::vector<MetadataRestoreSource> sources;
    std::map<std::string, std::size_t> indexByKey;
    for (const auto &proposal : proposals) {
        const std::string key = restoreSourceKey(proposal);
        auto found = indexByKey.find(key);
        if (found == indexByKey.end()) {
            MetadataRestoreSource source{key, proposal.storedFrom, 0};
            // Label-keyed while that label has recorded ids: rows
            // resolveRestoreSources() could not tie to one of them.
            source.idNotRecorded = key.rfind("label:", 0) == 0 && ids.count(proposal.storedFrom) > 0;
            found = indexByKey.emplace(key, sources.size()).first;
            sources.push_back(std::move(source));
        }
        sources[found->second].proposalCount++;
    }
    std::sort(sources.begin(), sources.end(), [](const MetadataRestoreSource &a, const MetadataRestoreSource &b) {
        return std::tie(a.label, a.key) < std::tie(b.label, b.key);
    });
    return sources;
}

std::map<std::string, int> restorePlaylistCounts(const std::vector<MetadataRestoreProposal> &proposals,
                                                 const std::string &sourceKey)
{
    std::map<std::string, int> counts;
    const MetadataRestoreScope scope{sourceKey, {}};
    for (const auto &proposal : proposals) {
        if (!proposalInRestoreScope(proposal, scope)) {
            continue;
        }
        for (const auto &name : restorePlaylistsOf(proposal)) {
            counts[name]++;
        }
    }
    return counts;
}

}  // namespace seabass::domain
