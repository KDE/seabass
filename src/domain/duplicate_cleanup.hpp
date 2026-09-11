#pragma once

#include <optional>
#include <vector>
#include <string>
#include <vector>

#include "domain/duplicate_cue_consolidation.hpp"
#include "domain/track.hpp"

namespace seabass::domain
{

// What to do about one DuplicateGroup for the "Clean Up" feature: which
// copy to keep, which to remove, and what data the survivor should end
// up with so nothing already on either copy is lost.
struct DuplicateCleanupPlan
{
    DuplicateGroup group;
    Track survivor;               // the copy that would be kept
    std::vector<Track> toRemove;  // every other copy in the group

    // Union of every copy's cues (via LocalRestorePlanner::mergeCues,
    // folded across the whole group), deduplicated -- what the survivor
    // should be written with so cleanup never loses a hot/memory cue
    // that only existed on a copy about to be removed.
    std::vector<CuePoint> mergedCuesForSurvivor;

    // "Fill a gap" propagation, not a merge: if the survivor is missing
    // bpm/key/artwork and some other copy in the group has it, that
    // value carries forward. Unlike cues (which can legitimately exist
    // on multiple copies and all need keeping), a track has exactly one
    // bpm/key/artwork -- so this is only ever "use the one value that
    // exists" not "reconcile several". Each optional is set only when
    // the survivor itself lacks the field AND some other copy has it;
    // the matching *DonorSourceId names which track it came from (empty
    // if nothing was propagated for that field), since some writers
    // (rekordbox's PdbRowWriter, OneLibraryCueWriter) copy the donor's
    // own already-valid field reference directly rather than
    // re-deriving one from the parsed value alone -- see
    // pdb_row_writer.hpp's copyTrackFieldsIfMissing() for why.
    std::optional<double> bpmForSurvivor;
    std::string bpmDonorSourceId;
    std::optional<std::string> keyForSurvivor;
    std::string keyDonorSourceId;
    std::optional<std::string> artworkPathForSurvivor;
    std::string artworkDonorSourceId;

    // True when picking by quality (bitrate) and picking by length
    // (duration) disagree on which copy is "best" -- e.g. the
    // highest-bitrate copy is also meaningfully shorter than another
    // copy. This can be entirely intentional (a DJ keeping a
    // lower-quality, shorter edit for a specific piece of hardware), so
    // callers should default a group like this to *excluded* from a
    // bulk cleanup and let the caller build a UI-facing explanation from
    // group.tracks' own bitrate/durationSeconds/fileSizeBytes rather
    // than have the domain layer format human text.
    bool differs = false;

    // Distinct from `differs` above: true when two or more copies in
    // the group carry a genuinely different rating or comment -- neither
    // of which this planner (or any writer in this codebase) propagates
    // onto the survivor, unlike bpm/key/artwork above. Removing the
    // other copies would silently and permanently lose whichever value
    // didn't happen to land on the survivor.
    //
    // Play counts and last-played timestamps are deliberately excluded,
    // see the comment at the assignment in duplicate_cleanup.cpp: they
    // are per-application counters that do not mean the same thing in
    // two different DJ applications, and including them made this flag
    // fire on 36 groups where 2 was the honest number. A group where only the survivor has a value
    // (nothing to lose) or every copy already agrees is NOT flagged --
    // this is specifically "real, differing, unpreservable data is
    // about to be discarded", not "some copy has more metadata than
    // another". Callers should default a group like this to *excluded*
    // too, same as `differs`, but with different UI text: this is about
    // per-copy DJ data (usage/opinion), not encode quality.
    bool hasUnpreservableDataAtRisk = false;

    // The unreferenced files (Track::isUnreferenced) among `toRemove`,
    // split into the ones this plan would delete and the ones it refuses
    // to. `toRemove` keeps its meaning -- every copy that is not the
    // survivor -- but the two kinds of copy are removed by entirely
    // different acts: a catalogued copy loses a database row and its
    // file stays, while a stray file has no row to drop, so removing it
    // means deleting the file. That is irreversible and no catalog will
    // ever mention the file again, so it is accounted for here rather
    // than left for each caller to infer from a flag on a Track.
    //
    // A stray is held back when the group is flagged `differs` (the
    // copies may be different edits, so the file may not be a duplicate
    // at all) or when any copy in the group had an estimated duration
    // (see Track::durationIsEstimated: the grouping itself is then in
    // doubt, which is a stronger reason than the one file's own length
    // being uncertain -- a wrongly grouped estimate could equally make
    // the *other* copies look redundant).
    //
    // `hasUnpreservableDataAtRisk` deliberately does NOT hold these
    // back, and that is a decision rather than an oversight. It protects
    // a rating or comment that only one copy carries; a stray file has
    // no row and so carries neither, and can only ever be caught by the
    // flag because two *catalogued* rows in its group disagree with each
    // other. Deleting the file loses none of the data the flag exists to
    // protect. On a real 3-catalog stick the coupling held back 2 of 632
    // stray files over a disagreement neither was party to -- and 57 of
    // them before play counts stopped counting as data at risk, which is
    // the shape of the mistake rather than its current size. Callers
    // still default such a group to excluded for the row-level cleanup,
    // exactly as before.
    std::vector<Track> unreferencedFilesToDelete;
    std::vector<Track> unreferencedFilesHeldBack;

    // True when some copy to remove is written in a format the survivor
    // is NOT written in, so dropping its row there would leave that
    // format with no row for this recording at all.
    //
    // This state is already a defect before cleanup touches it. The
    // formats are one library written three times and are meant to list
    // the same tracks (see Track::catalogRows), so a survivor missing
    // from one of them means they have diverged. Cleanup's job is not to
    // repair that, but it must not deepen it: removing the doomed row
    // would take the recording out of that format entirely, leaving the
    // formats further apart than it found them.
    //
    // Removing a row repoints that format's playlists at the surviving
    // file, which needs the survivor to have a row there. Where it does
    // not, the alternatives are to strand the format or to rewrite the
    // doomed row to point at the survivor's file -- and that second one
    // is a different and much larger feature (no writer here adds or
    // repoints a row's file reference; see
    // docs/deduplication-roadmap.md).
    //
    // Unlike `differs` and `hasUnpreservableDataAtRisk`, this is not a
    // judgement call to default away from and let the DJ override: there
    // is no correct way to apply such a group yet, so callers must
    // refuse to stage it rather than merely unchecking it. Measured on a
    // real stick carrying all three formats it never fires -- 235
    // groups, all covered -- which is what makes holding affordable
    // insurance rather than a limitation anyone will meet.
    bool wouldStrandAFormat = false;
};

// Decides survivor/removal/cue-merge for one DuplicateGroup. Only
// meaningful for groups with 2+ tracks -- DuplicateTrackFinder::find()
// never produces fewer, but a 1-track (or empty) group is handled
// harmlessly (survivor is that track, nothing to remove).
class DuplicateCleanupPlanner
{
public:
    static DuplicateCleanupPlan plan(const DuplicateGroup &group);
};

// Every catalog whose rows this plan's removals actually live in, in
// first-seen order and without repeats. Stray files contribute nothing:
// no catalog lists them, which is why they are strays.
//
// Applying a plan means removing all of these, and a caller that can
// write only some of them must refuse the whole plan rather than write
// the part it can. A file listed by three catalogs and removed from one
// leaves the other two pointing at audio the first no longer lists --
// the split state this feature exists to prevent, reached from the
// writing side instead of the planning side.
//
// Empty of anything beyond a track's own format until rows have been
// collapsed into files (application::collapseCatalogRows); until then
// each row is its own track and each plan touches one catalog.
std::vector<std::string> catalogsWrittenBy(const DuplicateCleanupPlan &plan);

// What ONE catalog's writer has to do for this plan: which of that
// catalog's rows to remove, and which row id in that same catalog the
// removed rows' playlists should be repointed at.
//
// Every id here belongs to `format` and to no other. That is the whole
// point: a file collapsed across catalogs carries a different row id in
// each, and handing rekordbox's id to Engine's writer either does
// nothing or hits an unrelated row. Ids are read from Track::catalogRows,
// falling back to the track's own sourceId when its own format matches --
// an uncollapsed row is still that catalog's row.
struct CatalogWriteTargets
{
    // Empty when this catalog has no row for the survivor at all.
    std::string survivorSourceId;
    std::vector<std::string> doomedSourceIds;
};

CatalogWriteTargets writeTargetsFor(const DuplicateCleanupPlan &plan, const std::string &format);

// The track's row id in `format`: its catalogRows entry for that format
// on a collapsed file, its own sourceId when it is a row of that format,
// empty otherwise. A writer must never be handed a track's base sourceId
// for a format it was not read from: rekordbox and Engine both number
// rows densely from 1, so a wrong-format id silently hits another track.
std::string rowIdIn(const Track &track, const std::string &format);

// False when this catalog has rows to remove but no row for the survivor
// to repoint at. Removing them anyway would drop the doomed rows'
// playlist entries on the floor, or repoint them at an id this catalog
// does not have -- so a caller must refuse rather than write.
//
// This is the writer-side view of the same condition the planner already
// records as DuplicateCleanupPlan::wouldStrandAFormat. Two views rather
// than one because they answer different questions at different times:
// the flag says "this plan cannot be staged at all", once, over every
// format; this says "this particular catalog cannot be written", per
// format, and comes with the ids that catalog's writer needs. They must
// never disagree, and a test asserts they do not -- see
// duplicate_cleanup_test's case pairing them, which exists precisely so
// the two cannot drift apart.
//
// A named predicate rather than the condition spelled out at each call
// site, because getting it inside out writes exactly the damage the
// refusal exists to prevent, and silently.
bool canWriteWholeCatalog(const CatalogWriteTargets &targets);

// True when this catalog has nothing to do for this plan: no row to
// remove and no survivor row to merge cues onto. Distinct from
// canWriteWholeCatalog() returning false, which means there IS work and
// it cannot be done safely.
bool hasNoWork(const CatalogWriteTargets &targets);

}  // namespace seabass::domain
