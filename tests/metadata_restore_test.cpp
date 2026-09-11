#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "domain/metadata_restore.hpp"

using seabass::domain::CuePoint;
using seabass::domain::MetadataRestoreProposal;
using seabass::domain::planMetadataRestore;
using seabass::domain::Track;

namespace
{

// The two dates every case below is decided against when nothing else
// separates the copies. Named for what they mean: whichever side was
// edited more recently wins the last step of the merge rule.
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
    // store outlives the stick, so a path is the one thing about a
    // stored row that cannot be trusted -- and matchTracks treats an
    // exact path match as decisive, which is why it must never get the
    // chance to fire here. Artist, title and length do the work.
    track.filePath.clear();
    track.metadataModifiedAt = StoredAt;
    return track;
}

const MetadataRestoreProposal *find(const std::vector<MetadataRestoreProposal> &proposals, const std::string &title)
{
    for (const auto &proposal : proposals) {
        if (proposal.stickTrack.title == title) {
            return &proposal;
        }
    }
    return nullptr;
}

}  // namespace

int main()
{
    // ---- the headline case: a stick track with no cues at all -------
    {
        Track stick = stickTrack("Erste");
        Track stored = storedTrack("Erste");
        stored.cues = {hotCue(1, 32000.0), hotCue(2, 64000.0)};

        // The stick was written more recently and still gets the cues:
        // it has none, so this is a blank being filled, and step one of
        // the rule settles it before any date is consulted.
        const auto proposals = planMetadataRestore({stick}, {stored}, StickWrittenRecently);
        assert(proposals.size() == 1);
        assert(proposals[0].cuesOffered);
        assert(proposals[0].cuesFillAGap);
        assert(!proposals[0].cuesConflict);
        assert(proposals[0].cues.size() == 2);
        assert(proposals[0].cuesAdded() == 2);
        // Matched with no path on the stored side at all.
        assert(proposals[0].storedId == "7");
        std::cout << "case 1 (no cues on the stick, cues in the store) OK\n";
    }

    // ---- a track that already has everything --------------------------
    {
        Track stick = stickTrack("Erste");
        stick.cues = {hotCue(1, 32000.0)};
        Track stored = storedTrack("Erste");
        stored.cues = {hotCue(1, 32000.0)};

        const auto proposals = planMetadataRestore({stick}, {stored}, StickWrittenLongAgo);
        // Not a decision anyone needs to make, so not on the list.
        assert(proposals.empty());
        std::cout << "case 2 (nothing to offer, nothing proposed) OK\n";
    }

    // ---- cues that differ: the larger set wins -------------------------
    {
        Track stick = stickTrack("Erste");
        stick.cues = {hotCue(1, 32000.0)};
        Track stored = storedTrack("Erste");
        stored.cues = {hotCue(1, 48000.0), hotCue(2, 64000.0)};

        // Two stored cues against one on the stick. More cues wins, and
        // it wins whichever side was written last: this is the case the
        // rule exists for, a re-export having left one cue where there
        // used to be several.
        for (const std::int64_t stickAt : {StickWrittenLongAgo, StickWrittenRecently}) {
            const auto proposals = planMetadataRestore({stick}, {stored}, stickAt);
            assert(proposals.size() == 1);
            assert(proposals[0].cuesOffered);
            assert(proposals[0].cuesConflict);
            assert(!proposals[0].cuesFillAGap);
            assert(proposals[0].cues.size() == 2);
        }
        std::cout << "case 3 (more cues wins, whenever each side was written) OK\n";
    }

    // ---- cues that differ with nothing to choose between them ----------
    {
        Track stick = stickTrack("Erste");
        stick.cues = {hotCue(1, 32000.0), hotCue(2, 90000.0)};
        Track stored = storedTrack("Erste");
        stored.cues = {hotCue(1, 48000.0), hotCue(2, 64000.0)};

        // Equal counts, so the rule falls through to the dates. A stick
        // re-cued since the backup keeps its own work.
        const auto recent = planMetadataRestore({stick}, {stored}, StickWrittenRecently);
        assert(recent.empty());

        // And a stick that has not been touched since takes the stored
        // set back.
        const auto stale = planMetadataRestore({stick}, {stored}, StickWrittenLongAgo);
        assert(stale.size() == 1);
        assert(stale[0].cuesOffered);
        assert(stale[0].cuesConflict);
        std::cout << "case 4 (equal counts fall through to the later edit) OK\n";
    }

    // ---- a conflict does not block the other fields -------------------
    {
        Track stick = stickTrack("Erste");
        stick.cues = {hotCue(1, 32000.0), hotCue(2, 90000.0)};  // conflicts, same count
        stick.comment = "";                                      // blank
        Track stored = storedTrack("Erste");
        stored.cues = {hotCue(1, 48000.0), hotCue(2, 64000.0)};
        stored.comment = "peak time";
        stored.rating = 4;

        const auto proposals = planMetadataRestore({stick}, {stored}, StickWrittenRecently);
        assert(proposals.size() == 1);
        // Cues kept: they conflict, the counts are equal, and the stick
        // was written last.
        assert(!proposals[0].cuesOffered);
        assert(proposals[0].cuesConflict);
        // The comment and rating land anyway: the stick has neither, and
        // filling a blank is not overwriting.
        assert(proposals[0].commentOffered);
        assert(proposals[0].comment == "peak time");
        assert(proposals[0].ratingOffered);
        assert(*proposals[0].rating == 4);
        std::cout << "case 5 (each field group decided on its own) OK\n";
    }

    // ---- a rating of 0 is a rating ------------------------------------
    {
        Track stick = stickTrack("Erste");
        stick.rating = 0;               // explicitly zero stars
        Track stored = storedTrack("Erste");
        stored.rating = 5;

        // Zero stars is a decision the DJ made, not an empty field, so
        // this is a disagreement rather than a blank and the dates
        // decide it. A stick rated since the backup keeps its zero.
        const auto recent = planMetadataRestore({stick}, {stored}, StickWrittenRecently);
        assert(recent.empty());

        const auto stale = planMetadataRestore({stick}, {stored}, StickWrittenLongAgo);
        assert(stale.size() == 1);
        assert(stale[0].ratingConflict);
        assert(*stale[0].rating == 5);
        std::cout << "case 6 (zero stars is a rating, not a blank) OK\n";
    }

    // ---- nothing is ever replaced with nothing -------------------------
    {
        Track stick = stickTrack("Erste");
        stick.cues = {hotCue(1, 32000.0)};
        stick.comment = "peak time";
        stick.rating = 4;
        // A store row that holds nothing at all, and a stick that has
        // not been written in years. The dates say the store is newer;
        // it still takes nothing away, because an empty field is a gap
        // in the store rather than an instruction to clear one.
        Track stored = storedTrack("Erste");

        const auto proposals = planMetadataRestore({stick}, {stored}, StickWrittenLongAgo);
        assert(proposals.empty());
        std::cout << "case 7 (an empty store never erases what is on the stick) OK\n";
    }

    // ---- a stick track the store has never seen -----------------------
    {
        Track stick = stickTrack("Unknown", "Nobody");
        Track stored = storedTrack("Erste");
        stored.cues = {hotCue(1, 1000.0)};

        const auto proposals = planMetadataRestore({stick}, {stored}, StickWrittenLongAgo);
        assert(proposals.empty());
        std::cout << "case 8 (an unmatched stick track is left alone) OK\n";
    }

    // ---- length keeps two mixes apart ---------------------------------
    {
        Track radioEdit = stickTrack("One Track");
        radioEdit.durationSeconds = 210.0;
        Track storedExtended = storedTrack("One Track");
        storedExtended.durationSeconds = 480.0;
        storedExtended.cues = {hotCue(1, 400000.0)};

        const auto proposals = planMetadataRestore({radioEdit}, {storedExtended}, StickWrittenLongAgo);
        // A cue 6:40 into a 3:30 track would be past the end of it.
        assert(proposals.empty());
        std::cout << "case 9 (a different length is a different track) OK\n";
    }

    // ---- matching survives what a path would not -----------------------
    {
        // The same recording, re-exported into a library that lays its
        // folders out differently and renames the file. A planner keyed
        // on paths would see two unrelated tracks; this one restores the
        // cues, which is the flexibility the store exists to provide.
        Track stick = stickTrack("Erste");
        stick.filePath = "/media/REBUILT/Contents/UnknownArtist/01 - Erste (Original Mix).mp3";
        stick.filename = "01 - Erste (Original Mix).mp3";
        Track stored = storedTrack("Erste");
        stored.cues = {hotCue(1, 32000.0)};

        const auto proposals = planMetadataRestore({stick}, {stored}, StickWrittenRecently);
        assert(proposals.size() == 1);
        assert(proposals[0].cuesOffered);
        std::cout << "case 10 (a renamed file on a rebuilt stick still matches) OK\n";
    }

    // ---- several tracks at once ---------------------------------------
    {
        Track first = stickTrack("Erste");
        Track second = stickTrack("Zweite");
        second.sourceId = "43";
        Track thirdWithCues = stickTrack("Dritte");
        thirdWithCues.sourceId = "44";
        thirdWithCues.cues = {hotCue(1, 1000.0)};

        Track storedFirst = storedTrack("Erste");
        storedFirst.cues = {hotCue(1, 1000.0)};
        Track storedThird = storedTrack("Dritte");
        storedThird.sourceId = "9";
        storedThird.cues = {hotCue(1, 1000.0)};

        const auto proposals =
            planMetadataRestore({first, second, thirdWithCues}, {storedFirst, storedThird}, StickWrittenLongAgo);
        assert(proposals.size() == 1);
        assert(find(proposals, "Erste") != nullptr);
        assert(find(proposals, "Zweite") == nullptr);  // the store has never seen it
        assert(find(proposals, "Dritte") == nullptr);  // already has the same cue
        std::cout << "case 11 (only the tracks with something to gain) OK\n";
    }

    // ---- what each format can actually take ---------------------------
    //
    // The planner offers a comment whenever the store has one and the
    // stick does not; which formats can store it is the writer's
    // business. This pins the fact the page depends on: a proposal
    // carries the catalog rows that decide it, so a track catalogued
    // only in DeviceLibrary is distinguishable from one Engine also
    // holds. See tests/pdb_rating_write_test.cpp for why that matters.
    {
        Track rekordboxOnly = stickTrack("Erste");
        rekordboxOnly.catalogRows = {{"rekordbox", "42"}};
        Track alsoEngine = stickTrack("Zweite");
        alsoEngine.sourceId = "43";
        alsoEngine.catalogRows = {{"rekordbox", "43"}, {"engine", "900"}};

        Track storedFirst = storedTrack("Erste");
        storedFirst.comment = "peak time";
        storedFirst.rating = 4;
        Track storedSecond = storedTrack("Zweite");
        storedSecond.sourceId = "8";
        storedSecond.comment = "closer";

        const auto proposals =
            planMetadataRestore({rekordboxOnly, alsoEngine}, {storedFirst, storedSecond}, StickWrittenLongAgo);
        assert(proposals.size() == 2);

        const auto *first = find(proposals, "Erste");
        assert(first && first->commentOffered);
        assert(first->stickTrack.catalogRows.size() == 1);
        assert(first->stickTrack.catalogRows[0].format == "rekordbox");
        // A rating goes back even on this one: export.pdb stores it in a
        // byte that is already there.
        assert(first->ratingOffered && *first->rating == 4);

        const auto *second = find(proposals, "Zweite");
        assert(second && second->commentOffered);
        bool hasEngine = false;
        for (const auto &row : second->stickTrack.catalogRows) {
            if (row.format == "engine") {
                hasEngine = true;
            }
        }
        assert(hasEngine);
        std::cout << "case 12 (a proposal carries the catalogs that decide what can be written) OK\n";
    }

    std::cout << "all metadata_restore_test cases passed\n";
    return 0;
}
