#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "domain/metadata_backup_plan.hpp"

using seabass::domain::CuePoint;
using seabass::domain::MetadataBackupPlan;
using seabass::domain::MetadataBackupProposal;
using seabass::domain::planMetadataBackup;
using seabass::domain::Track;

namespace
{

// The dates each case below is decided against when nothing else
// separates the two copies. Named for what they mean: the last step of
// the merge rule gives it to whichever side was edited more recently.
constexpr std::int64_t StickWrittenLongAgo = 1'700'000'000;
constexpr std::int64_t StickWrittenRecently = 1'900'000'000;
constexpr std::int64_t StoredAt = 1'800'000'000;

CuePoint hotCue(int number, double positionMs)
{
    CuePoint cue;
    cue.kind = CuePoint::Kind::Hot;
    cue.hotCueNumber = number;
    cue.positionMs = positionMs;
    cue.color = "#FF0000";
    return cue;
}

Track stickTrack(const std::string &title, const std::string &artist = "Kalte Nacht")
{
    Track track;
    track.format = "rekordbox";
    track.sourceId = "42";
    track.title = title;
    track.artist = artist;
    track.filename = title + ".mp3";
    track.filePath = "/media/RV2/Contents/" + artist + "/" + title + ".mp3";
    track.durationSeconds = 361.5;
    return track;
}

Track storedTrack(const std::string &title, const std::string &artist = "Kalte Nacht")
{
    Track track = stickTrack(title, artist);
    track.format = "metadata-store";
    track.sourceId = "7";
    // No path at all, exactly as MetadataStore::readAll leaves it. The
    // store outlives the stick it was filled from, so a path is the one
    // thing about a stored row that cannot be trusted -- and matchTracks
    // treats an exact path match as decisive, which is why it must never
    // get the chance to fire here.
    track.filePath.clear();
    track.metadataModifiedAt = StoredAt;
    return track;
}

const MetadataBackupProposal &only(const MetadataBackupPlan &plan)
{
    assert(plan.proposals.size() == 1);
    return plan.proposals.front();
}

}  // namespace

int main()
{
    // ---- new to the store ------------------------------------------
    {
        Track stick = stickTrack("Zwielicht");
        stick.cues = {hotCue(1, 1000.0), hotCue(2, 2000.0)};
        stick.rating = 4;
        stick.comment = "peak time";

        const auto plan = planMetadataBackup({stick}, {}, StickWrittenLongAgo);
        const auto &p = only(plan);
        assert(p.isNew);
        assert(p.storedId.empty());
        assert(p.offersAnything());
        assert(p.cuesOffered && p.ratingOffered && p.commentOffered);
        // Nothing to conflict with: a track the store has never seen is
        // one fact, not three disagreements.
        assert(!p.cuesConflict && !p.ratingConflict && !p.commentConflict);
        assert(p.cuesFillAGap);
        // Measured against nothing, so every cue on it counts as added.
        assert(p.cuesAdded() == 2);
        assert(plan.tracksSeen == 1);
        assert(plan.alreadyCurrent == 0);
        std::cout << "case 1 (new to the store: everything is offered, nothing conflicts) OK\n";
    }

    // ---- already current -------------------------------------------
    {
        Track stick = stickTrack("Zwielicht");
        stick.cues = {hotCue(1, 1000.0)};
        stick.rating = 4;
        Track stored = storedTrack("Zwielicht");
        stored.cues = {hotCue(1, 1000.0)};
        stored.rating = 4;

        const auto plan = planMetadataBackup({stick}, {stored}, StickWrittenLongAgo);
        // The whole point of the list: a backup that would change
        // nothing about this track is not a decision anyone needs to
        // make, so it is counted and not shown.
        assert(plan.proposals.empty());
        assert(plan.alreadyCurrent == 1);
        assert(plan.tracksSeen == 1);
        std::cout << "case 2 (identical copies are counted, not listed) OK\n";
    }

    // ---- the store fills a gap --------------------------------------
    {
        Track stick = stickTrack("Zwielicht");
        stick.cues = {hotCue(1, 1000.0), hotCue(2, 2000.0), hotCue(3, 3000.0)};
        Track stored = storedTrack("Zwielicht");  // no cues at all

        const auto plan = planMetadataBackup({stick}, {stored}, StickWrittenLongAgo);
        const auto &p = only(plan);
        assert(!p.isNew);
        assert(p.storedId == "7");
        assert(p.cuesOffered);
        assert(p.cuesFillAGap);
        assert(!p.cuesConflict);
        assert(p.cuesAdded() == 3);
        std::cout << "case 3 (stored row has no cues: the stick's three fill the gap) OK\n";
    }

    // ---- more cues beats fewer, whichever side is newer --------------
    {
        Track stick = stickTrack("Zwielicht");
        stick.cues = {hotCue(1, 1000.0), hotCue(2, 2000.0), hotCue(3, 3000.0), hotCue(4, 4000.0)};
        Track stored = storedTrack("Zwielicht");
        stored.cues = {hotCue(1, 5000.0)};

        // Step 2 of the merge rule decides before the dates are even
        // consulted, so the older stick still wins on four cues to one.
        const auto plan = planMetadataBackup({stick}, {stored}, StickWrittenLongAgo);
        const auto &p = only(plan);
        assert(p.cuesOffered);
        assert(p.cuesConflict);
        assert(!p.cuesFillAGap);
        assert(p.cuesAdded() == 3);  // four after, one before
        std::cout << "case 4 (four cues beat one even from the older side) OK\n";
    }

    // ---- and the stored copy can win --------------------------------
    {
        Track stick = stickTrack("Zwielicht");
        stick.cues = {hotCue(1, 1000.0)};
        Track stored = storedTrack("Zwielicht");
        stored.cues = {hotCue(1, 5000.0), hotCue(2, 6000.0), hotCue(3, 7000.0)};

        const auto plan = planMetadataBackup({stick}, {stored}, StickWrittenRecently);
        // Nothing else on the track, so with the cues refused there is
        // nothing left to offer and the row does not reach the list.
        assert(plan.proposals.empty());
        assert(plan.alreadyCurrent == 1);
        std::cout << "case 5 (stored cue set wins: the row is left off the list) OK\n";
    }

    // ---- field groups are decided separately -------------------------
    {
        Track stick = stickTrack("Zwielicht");
        stick.cues = {hotCue(1, 1000.0)};
        stick.comment = "needs a re-rip";
        Track stored = storedTrack("Zwielicht");
        stored.cues = {hotCue(1, 5000.0), hotCue(2, 6000.0), hotCue(3, 7000.0)};

        const auto plan = planMetadataBackup({stick}, {stored}, StickWrittenRecently);
        const auto &p = only(plan);
        // The cues lost, and the comment the store never had still goes
        // in. A track is not one decision.
        assert(!p.cuesOffered);
        assert(p.commentOffered);
        assert(p.offersAnything());
        assert(p.cuesAdded() == 0);
        std::cout << "case 6 (cues refused, comment still offered) OK\n";
    }

    // ---- a conflict the stick wins on recency ------------------------
    {
        Track stick = stickTrack("Zwielicht");
        stick.rating = 5;
        Track stored = storedTrack("Zwielicht");
        stored.rating = 2;

        const auto newer = planMetadataBackup({stick}, {stored}, StickWrittenRecently);
        assert(only(newer).ratingOffered);
        assert(only(newer).ratingConflict);

        const auto older = planMetadataBackup({stick}, {stored}, StickWrittenLongAgo);
        // Same disagreement, older stick: the stored rating stays and
        // the row drops off the list entirely.
        assert(older.proposals.empty());
        assert(older.alreadyCurrent == 1);
        std::cout << "case 7 (a disagreeing rating follows the dates both ways) OK\n";
    }

    // ---- nothing to recognise it by ----------------------------------
    {
        Track anonymous;
        anonymous.format = "rekordbox";
        anonymous.durationSeconds = 200.0;
        // No title, no artist, no filename: a row nothing could ever
        // match again, which the store itself refuses to write.
        const auto plan = planMetadataBackup({anonymous}, {}, StickWrittenLongAgo);
        assert(plan.proposals.empty());
        assert(plan.withoutIdentity == 1);
        assert(plan.tracksSeen == 1);
        assert(plan.alreadyCurrent == 0);
        std::cout << "case 8 (no title, artist or filename: counted, never offered) OK\n";
    }

    {
        Track streaming = stickTrack("Zwielicht");
        streaming.streamingSource = "tidal";
        streaming.cues = {hotCue(1, 1000.0)};
        const auto plan = planMetadataBackup({streaming}, {}, StickWrittenLongAgo);
        // A streaming link names no file on any stick, so there is
        // nothing a restore could ever put its cues back on.
        assert(plan.proposals.empty());
        assert(plan.withoutIdentity == 1);
        std::cout << "case 9 (a streaming link is not a file to back up) OK\n";
    }

    // ---- the list keeps the stick's own order ------------------------
    {
        Track first = stickTrack("Aufbruch");
        first.cues = {hotCue(1, 1000.0)};
        Track second = stickTrack("Bernstein");
        second.filename = "Bernstein.mp3";
        second.cues = {hotCue(1, 1000.0)};
        Track third = stickTrack("Dunkelheit");
        third.cues = {hotCue(1, 1000.0)};

        // The middle one is already stored and current; the outer two
        // are new. A plan that bunched new tracks at one end would
        // reorder the list under the user for no reason.
        Track storedSecond = storedTrack("Bernstein");
        storedSecond.cues = {hotCue(1, 1000.0)};

        const auto plan = planMetadataBackup({first, second, third}, {storedSecond}, StickWrittenLongAgo);
        assert(plan.proposals.size() == 2);
        assert(plan.proposals[0].stickTrack.title == "Aufbruch");
        assert(plan.proposals[1].stickTrack.title == "Dunkelheit");
        assert(plan.tracksSeen == 3);
        assert(plan.alreadyCurrent == 1);
        std::cout << "case 10 (proposals come back in the stick's own order) OK\n";
    }

    std::cout << "metadata_backup_plan_test: all cases passed\n";
    return 0;
}
