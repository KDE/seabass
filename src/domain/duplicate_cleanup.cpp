#include "domain/duplicate_cleanup.hpp"

#include <algorithm>
#include <cmath>

#include "domain/local_restore.hpp"

namespace seabass::domain
{

namespace
{

// Matches sync_planning's/track_matching's own duration-tolerance
// convention: two lengths this close are "the same track", not a
// meaningfully different edit.
constexpr double DurationToleranceSeconds = 2.0;

bool durationsAgree(double a, double b)
{
    return std::abs(a - b) <= DurationToleranceSeconds;
}

// Index (into `tracks`) of the highest-scoring track among `candidates`,
// ties broken by longer duration, then larger file size, then original
// order -- deterministic regardless of scan order.
//
// Takes a candidate list rather than scoring the whole group because the
// survivor is sometimes chosen from a subset (see the "survivor must be
// catalogued" rule below) while `differs` still compares against the
// best of the *whole* group. Both need the identical tie-break, so there
// is one implementation and the caller says which tracks may win.
template<typename Score>
size_t bestOf(const std::vector<Track> &tracks, const std::vector<size_t> &candidates, Score score)
{
    size_t best = candidates.front();
    for (size_t c = 1; c < candidates.size(); ++c) {
        size_t i = candidates[c];
        auto scoreI = score(tracks[i]);
        auto scoreBest = score(tracks[best]);
        if (scoreI != scoreBest) {
            if (scoreI > scoreBest) {
                best = i;
            }
            continue;
        }
        if (!durationsAgree(tracks[i].durationSeconds, tracks[best].durationSeconds)) {
            if (tracks[i].durationSeconds > tracks[best].durationSeconds) {
                best = i;
            }
            continue;
        }
        if (tracks[i].fileSizeBytes > tracks[best].fileSizeBytes) {
            best = i;
        }
    }
    return best;
}

// True if some track about to be removed carries a value `get` doesn't
// find on the survivor -- checked against the survivor specifically,
// NOT "are there 2+ distinct values anywhere in the group": a group
// where only ONE doomed copy (not the survivor) has a value, and every
// other copy including the survivor has none, has exactly one distinct
// value in the whole group, but removing that doomed copy still
// silently discards the only copy of it. That's a real loss and must be
// flagged, even though nothing in the group technically "disagrees".
template<typename Get>
bool losesDataFromRemoval(const Track &survivor, const std::vector<Track> &toRemove, Get get)
{
    auto survivorValue = get(survivor);
    for (const auto &doomed : toRemove) {
        auto doomedValue = get(doomed);
        if (doomedValue.has_value() && doomedValue != survivorValue) {
            return true;
        }
    }
    return false;
}

}  // namespace

DuplicateCleanupPlan DuplicateCleanupPlanner::plan(const DuplicateGroup &group)
{
    DuplicateCleanupPlan result;
    result.group = group;

    if (group.tracks.empty()) {
        return result;
    }
    if (group.tracks.size() == 1) {
        result.survivor = group.tracks[0];
        result.mergedCuesForSurvivor = group.tracks[0].cues;
        return result;
    }

    bool anyBitrateKnown = std::any_of(group.tracks.begin(), group.tracks.end(),
                                        [](const Track &t) { return t.bitrate > 0; });

    std::vector<size_t> everyCopy(group.tracks.size());
    for (size_t i = 0; i < group.tracks.size(); ++i) {
        everyCopy[i] = i;
    }

    // An unreferenced file may be the survivor only when every copy in
    // the group is unreferenced.
    //
    // Both halves matter. Keeping a stray file over a catalogued copy
    // would leave the catalog pointing at the file we then delete -- to
    // do that safely the row would have to be repointed at the survivor,
    // which is a different and much larger feature. But a group of only
    // stray files is a real case (several copies of a track that fell
    // out of every catalog), and collapsing those to the best one is
    // exactly the right answer; what is left is then a candidate for
    // re-import, not for deletion.
    std::vector<size_t> catalogued;
    for (size_t i = 0; i < group.tracks.size(); ++i) {
        if (!group.tracks[i].isUnreferenced) {
            catalogued.push_back(i);
        }
    }
    const std::vector<size_t> &eligible = catalogued.empty() ? everyCopy : catalogued;

    size_t byDuration = bestOf(group.tracks, everyCopy, [](const Track &t) { return t.durationSeconds; });
    size_t survivorIndex = anyBitrateKnown
                               ? bestOf(group.tracks, eligible, [](const Track &t) { return t.bitrate; })
                               : bestOf(group.tracks, eligible, [](const Track &t) { return t.durationSeconds; });

    // "Differs" means picking by quality and picking by length actually
    // disagree, not merely that bitrates/sizes vary slightly (real
    // duplicate encodes of the same rip commonly do) -- only a
    // meaningfully different *duration* is a signal this might be a
    // deliberately different edit rather than just a different
    // encoding of the same audio.
    result.differs =
        anyBitrateKnown && survivorIndex != byDuration &&
        !durationsAgree(group.tracks[survivorIndex].durationSeconds, group.tracks[byDuration].durationSeconds);

    result.survivor = group.tracks[survivorIndex];
    for (size_t i = 0; i < group.tracks.size(); ++i) {
        if (i != survivorIndex) {
            result.toRemove.push_back(group.tracks[i]);
        }
    }

    std::vector<CuePoint> merged = result.survivor.cues;
    for (size_t i = 0; i < group.tracks.size(); ++i) {
        if (i == survivorIndex) {
            continue;
        }
        merged = LocalRestorePlanner::mergeCues(merged, group.tracks[i].cues);
    }
    result.mergedCuesForSurvivor = std::move(merged);

    // Fill-a-gap propagation: only when the survivor itself lacks the
    // field, and only the first donor found (deterministic group order)
    // -- there's exactly one bpm/key/artwork to end up with, not several
    // to reconcile, unlike cues above.
    if (result.survivor.bpm <= 0.0) {
        for (size_t i = 0; i < group.tracks.size(); ++i) {
            if (i != survivorIndex && group.tracks[i].bpm > 0.0) {
                result.bpmForSurvivor = group.tracks[i].bpm;
                result.bpmDonorSourceId = group.tracks[i].sourceId;
                break;
            }
        }
    }
    if (result.survivor.key.empty()) {
        for (size_t i = 0; i < group.tracks.size(); ++i) {
            if (i != survivorIndex && !group.tracks[i].key.empty()) {
                result.keyForSurvivor = group.tracks[i].key;
                result.keyDonorSourceId = group.tracks[i].sourceId;
                break;
            }
        }
    }
    if (result.survivor.artworkPath.empty()) {
        for (size_t i = 0; i < group.tracks.size(); ++i) {
            if (i != survivorIndex && !group.tracks[i].artworkPath.empty()) {
                result.artworkPathForSurvivor = group.tracks[i].artworkPath;
                result.artworkDonorSourceId = group.tracks[i].sourceId;
                break;
            }
        }
    }

    // Real, currently-unpreservable per-copy data: rating/comment/
    // playCount/lastPlayedAt are never propagated by this planner or any
    // writer, unlike bpm/key/artwork above -- so a genuine disagreement
    // here means removing the other copies really would discard one of
    // these values with no way to keep it.
    bool ratingLoses = losesDataFromRemoval(result.survivor, result.toRemove,
                                             [](const Track &t) -> std::optional<int> { return t.rating; });
    bool commentLoses =
        losesDataFromRemoval(result.survivor, result.toRemove, [](const Track &t) -> std::optional<std::string> {
            return t.comment.empty() ? std::nullopt : std::optional<std::string>(t.comment);
        });
    // playCount and lastPlayedAt are deliberately NOT part of this.
    //
    // They used to be, and it made the flag fire almost everywhere:
    // measured on a real 3-catalog stick, including them held back 57
    // audio files across 36 groups, against 2 files across 2 groups
    // without them. Nearly every one of those was a rekordbox row
    // carrying a play count meeting an Engine row carrying only a
    // last-played timestamp -- the two applications simply count
    // different things, so "they disagree" was being read off a
    // comparison that never had meaning.
    //
    // A play count belongs to the application that kept it. Merging one
    // across library types is not a thing that can be done correctly, and
    // it is not valuable enough to hold a cleanup hostage over. Within a
    // single library type the honest answer would be to add the counts
    // up, which is a real intent -- but no writer in this project can
    // write a play count into any of the three formats today, so that is
    // a follow-up needing a write path, not something to pretend at here.
    // The Clean Up page says so in as many words rather than leaving it
    // to be discovered.
    //
    // rating and comment stay: both are the DJ's own deliberate input,
    // both mean the same thing in every format, and losing one silently
    // is a real loss.
    result.hasUnpreservableDataAtRisk = ratingLoses || commentLoses;

    // Every format a doomed copy is written in must also carry the
    // survivor, or removing that copy's row strands the format -- see
    // the header. Checked over catalogRows, which only a caller that has
    // collapsed rows into files sets; a caller working in one format at
    // a time leaves it empty and this never fires.
    for (const auto &doomed : result.toRemove) {
        for (const auto &row : doomed.catalogRows) {
            bool survivorListedThere =
                std::any_of(result.survivor.catalogRows.begin(), result.survivor.catalogRows.end(),
                             [&row](const CatalogRowRef &s) { return s.format == row.format; });
            if (!survivorListedThere) {
                result.wouldStrandAFormat = true;
            }
        }
    }

    // Which stray files this plan would actually delete. Deliberately
    // computed *after* hasUnpreservableDataAtRisk and deliberately not
    // consulting it: see the header for why a flag about two catalog
    // rows disagreeing says nothing about a file that has no row.
    //
    // The two things that do hold a file back are both about the group's
    // identity rather than its data. `differs` says quality and length
    // disagree on which copy is best, which is how a deliberately
    // different edit shows up -- these may not be copies of one track at
    // all. An estimated duration says the same thing more quietly: the
    // group was formed on a length that was guessed from a bitrate, so
    // any file in it might not belong. That is why one estimate holds
    // back every stray in the group and not just the estimated file --
    // the doubt is about the grouping, and the file the guess dragged in
    // could as easily make the *others* look redundant.
    bool anyDurationEstimated = std::any_of(group.tracks.begin(), group.tracks.end(),
                                             [](const Track &t) { return t.durationIsEstimated; });
    for (const auto &doomed : result.toRemove) {
        if (!doomed.isUnreferenced) {
            continue;  // a catalog row is dropped, not a file deleted
        }
        if (result.differs || anyDurationEstimated) {
            result.unreferencedFilesHeldBack.push_back(doomed);
        } else {
            result.unreferencedFilesToDelete.push_back(doomed);
        }
    }

    return result;
}

std::string rowIdIn(const Track &track, const std::string &format)
{
    for (const auto &row : track.catalogRows) {
        if (row.format == format) {
            return row.sourceId;
        }
    }
    // An uncollapsed track carries no catalogRows but is still a row in
    // its own catalog. Checked after catalogRows rather than before, so
    // a collapsed file's explicit per-format id always wins over the
    // format its representative row happened to be read from.
    if (track.format == format) {
        return track.sourceId;
    }
    return {};
}

CatalogWriteTargets writeTargetsFor(const DuplicateCleanupPlan &plan, const std::string &format)
{
    CatalogWriteTargets targets;
    targets.survivorSourceId = rowIdIn(plan.survivor, format);
    for (const auto &doomed : plan.toRemove) {
        // A stray has no catalog row anywhere -- its sourceId is a file
        // path, which no writer would recognise. It is routed to the
        // pending-deletion manifest instead, never to a catalog writer.
        if (doomed.isUnreferenced) {
            continue;
        }
        std::string id = rowIdIn(doomed, format);
        if (!id.empty()) {
            targets.doomedSourceIds.push_back(id);
        }
    }
    return targets;
}

bool canWriteWholeCatalog(const CatalogWriteTargets &targets)
{
    return targets.doomedSourceIds.empty() || !targets.survivorSourceId.empty();
}

bool hasNoWork(const CatalogWriteTargets &targets)
{
    return targets.survivorSourceId.empty() && targets.doomedSourceIds.empty();
}

std::vector<std::string> catalogsWrittenBy(const DuplicateCleanupPlan &plan)
{
    std::vector<std::string> catalogs;
    auto add = [&catalogs](const std::string &format) {
        if (format.empty()) {
            return;
        }
        if (std::find(catalogs.begin(), catalogs.end(), format) == catalogs.end()) {
            catalogs.push_back(format);
        }
    };
    for (const auto &doomed : plan.toRemove) {
        if (doomed.isUnreferenced) {
            continue;
        }
        // The track's own format counts even when catalogRows is empty:
        // an uncollapsed row is still a row in a catalog.
        add(doomed.format);
        for (const auto &row : doomed.catalogRows) {
            add(row.format);
        }
    }
    return catalogs;
}

}  // namespace seabass::domain
