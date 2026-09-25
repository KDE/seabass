// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "domain/metadata_restore.hpp"

using seabass::domain::CuePoint;
using seabass::domain::MetadataRestoreProposal;
using seabass::domain::MetadataRestoreScope;
using seabass::domain::PlaylistMembership;
using seabass::domain::proposalInRestoreScope;
using seabass::domain::resolveRestoreSources;
using seabass::domain::restorePlaylistCounts;
using seabass::domain::restoreSourceKey;
using seabass::domain::restoreSources;
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

    // 13. A stray memory cue at 0:00 is a fault, not work anyone did. A
    //     restore must never put one back -- backups taken before this
    //     was filtered on the way in still hold them -- and a stick
    //     carrying nothing but strays has no cues to weigh against, so
    //     the store fills a gap rather than fighting a conflict.
    {
        CuePoint stray;
        stray.kind = CuePoint::Kind::Memory;
        stray.positionMs = 340;  // what Engine's own analysis leaves

        Track stick = stickTrack("Nur Ein Moment");
        stick.cues = {stray};
        Track stored = storedTrack("Nur Ein Moment");
        stored.cues = {hotCue(1, 30'000), stray};

        const auto proposals = planMetadataRestore({stick}, {stored}, StoredAt - 1000);
        const MetadataRestoreProposal *proposal = find(proposals, "Nur Ein Moment");
        assert(proposal != nullptr);
        assert(proposal->cuesOffered);
        assert(proposal->cuesFillAGap && "a stick with only strays has no cues to keep");
        assert(!proposal->cuesConflict);
        assert(proposal->cues.size() == 1 && "the stray is not written back");
        assert(proposal->cues.front().kind == CuePoint::Kind::Hot);
        assert(proposal->cuesAdded() == 1 && "and the count is of cues that are really added");

        // A hot cue at the start is noise on the same terms, so a
        // restore does not put one back either; one further in does.
        Track stickTwo = stickTrack("Am Anfang");
        Track storedTwo = storedTrack("Am Anfang");
        storedTwo.cues = {hotCue(1, 0), hotCue(2, 45'000.0)};
        const auto more = planMetadataRestore({stickTwo}, {storedTwo}, StoredAt - 1000);
        const MetadataRestoreProposal *second = find(more, "Am Anfang");
        assert(second != nullptr && second->cuesOffered);
        assert(second->cues.size() == 1 && "only the one that is really a cue");
        assert(second->cues.front().positionMs == 45'000.0);
        std::cout << "case 13 (stray cues are not restored, and a stick holding only strays counts as empty) OK\n";
    }

    // ---- case 14: the backup's playlists travel onto the proposal -----
    //
    // What the restore page's playlist picker narrows on. The stored
    // side's record, because a stick that lost its cues after a rebuild
    // may have lost its playlists with them.
    {
        Track stick = stickTrack("Neonlicht");
        Track stored = storedTrack("Neonlicht");
        stored.cues = {hotCue(1, 12'000.0)};
        stored.playlists = {PlaylistMembership{"Techno/Peak Time", 3}, PlaylistMembership{"Warm Up", 1},
                            PlaylistMembership{"Warm Up", 7}};
        const auto proposals = planMetadataRestore({stick}, {stored}, StickWrittenLongAgo);
        const MetadataRestoreProposal *proposal = find(proposals, "Neonlicht");
        assert(proposal != nullptr);
        assert(proposal->storedPlaylists.size() == 2 && "one entry per playlist, however often it lists the track");
        assert(proposal->storedPlaylists[0] == "Techno/Peak Time");
        assert(proposal->storedPlaylists[1] == "Warm Up");
        std::cout << "case 14 (the backup's playlists travel onto the proposal) OK\n";
    }

    // ---- case 15: scoping a restore to one stick's backup ---------------
    //
    // Keyed on the stick's identity, not its label: two sticks both called
    // NO NAME are the ordinary case, and a restore narrowed to one of them
    // must not take the other's tracks along.
    {
        const auto from = [](const std::string &title, const std::string &libraryId, const std::string &label) {
            MetadataRestoreProposal proposal;
            proposal.stickTrack = stickTrack(title);
            proposal.storedFrom = label;
            proposal.storedFromLibraryId = libraryId;
            proposal.cuesOffered = true;
            return proposal;
        };
        const std::vector<MetadataRestoreProposal> proposals = {
            from("A", "uuid-1", "NO NAME"), from("B", "uuid-2", "NO NAME"), from("C", "uuid-1", "NO NAME"),
            from("D", "", "RV2"),           from("E", "", ""),
        };
        assert(restoreSourceKey(proposals[0]) != restoreSourceKey(proposals[1])
               && "two sticks sharing a label are two sticks");
        assert(restoreSourceKey(proposals[3]) == "label:RV2" && "the label stands in when no id was recorded");
        // A row the store knows neither the id nor the label of is still a
        // stick of its own in the picker, and its key is not the empty
        // one, which means "every stick": picking it used to select
        // everything while the picker said "A stick with no name".
        assert(!restoreSourceKey(proposals[4]).empty() && "empty is the key of every stick");
        assert(restoreSourceKey(proposals[4]) != restoreSourceKey(proposals[3]));
        {
            const MetadataRestoreScope unnamed{restoreSourceKey(proposals[4]), {}};
            int inUnnamed = 0;
            for (const auto &proposal : proposals) {
                inUnnamed += proposalInRestoreScope(proposal, unnamed) ? 1 : 0;
            }
            assert(inUnnamed == 1 && "the unidentified stick's scope is E alone, not every stick");
            assert(proposalInRestoreScope(proposals[4], unnamed));
        }
        const MetadataRestoreScope first{restoreSourceKey(proposals[0]), {}};
        int inFirst = 0;
        for (const auto &proposal : proposals) {
            inFirst += proposalInRestoreScope(proposal, first) ? 1 : 0;
        }
        assert(inFirst == 2 && "A and C, and not B from the other NO NAME");
        assert(proposalInRestoreScope(proposals[1], MetadataRestoreScope{}) && "no scope keeps everything");

        const auto sources = restoreSources(proposals);
        assert(sources.size() == 4);
        // Sorted by label, then key: "" first, the two NO NAMEs by id, RV2.
        assert(sources[0].label.empty() && sources[0].proposalCount == 1);
        assert(sources[0].key == restoreSourceKey(proposals[4]) && !sources[0].key.empty());
        assert(sources[1].label == "NO NAME" && sources[1].key == "id:uuid-1" && sources[1].proposalCount == 2);
        assert(sources[2].label == "NO NAME" && sources[2].key == "id:uuid-2" && sources[2].proposalCount == 1);
        assert(sources[3].label == "RV2" && sources[3].proposalCount == 1);
        std::cout << "case 15 (a restore scoped to one stick's backup, keyed on the stick not its label) OK\n";
    }

    // ---- case 16: scoping a restore to one playlist ----------------------
    //
    // A track is in a playlist when the backup recorded it there or the
    // stick lists it there. The counts are of proposals, per stick.
    {
        const auto proposal = [](const std::string &title, const std::string &libraryId,
                                 std::vector<std::string> storedPlaylists, std::vector<std::string> stickPlaylists) {
            MetadataRestoreProposal p;
            p.stickTrack = stickTrack(title);
            for (const auto &name : stickPlaylists) {
                p.stickTrack.playlists.push_back(PlaylistMembership{name, 1});
            }
            p.storedPlaylists = std::move(storedPlaylists);
            p.storedFromLibraryId = libraryId;
            p.cuesOffered = true;
            return p;
        };
        const std::vector<MetadataRestoreProposal> proposals = {
            proposal("Stored only", "uuid-1", {"Warm Up"}, {}),
            proposal("Stick only", "uuid-1", {}, {"Warm Up"}),
            proposal("Both", "uuid-2", {"Warm Up"}, {"Warm Up", "Closing"}),
            proposal("Neither", "uuid-2", {"Closing"}, {}),
        };
        const MetadataRestoreScope warmUp{{}, "Warm Up"};
        assert(proposalInRestoreScope(proposals[0], warmUp) && "the backup's record counts");
        assert(proposalInRestoreScope(proposals[1], warmUp) && "and so does the stick's own");
        assert(proposalInRestoreScope(proposals[2], warmUp));
        assert(!proposalInRestoreScope(proposals[3], warmUp));
        // Both narrowings at once: one stick's backup AND one playlist.
        const MetadataRestoreScope both{"id:uuid-2", "Warm Up"};
        assert(!proposalInRestoreScope(proposals[0], both) && "right playlist, wrong stick");
        assert(proposalInRestoreScope(proposals[2], both));
        assert(!proposalInRestoreScope(proposals[3], both) && "right stick, wrong playlist");

        const auto all = restorePlaylistCounts(proposals, "");
        assert(all.size() == 2);
        assert(all.at("Warm Up") == 3 && "counted once per proposal, whichever side lists it");
        assert(all.at("Closing") == 2 && "a track both sides list there is one track");
        const auto second = restorePlaylistCounts(proposals, "id:uuid-2");
        assert(second.at("Warm Up") == 1 && second.at("Closing") == 2 && "counted within the picked stick");
        std::cout << "case 16 (a restore scoped to one playlist, as the backup or the stick lists it) OK\n";
    }

    // ---- case 17: one stick whose rows are only partly stamped ----------
    //
    // The store recorded a stick's library id only from some point on, so
    // one stick's older rows carry just its label. They are the same
    // stick: one picker entry, and picking it restores all of them. Only
    // when two recorded sticks share the label is there no telling which
    // an unstamped row belongs to, and then those rows stay apart, marked
    // so the page can say so.
    {
        const auto from = [](const std::string &title, const std::string &libraryId, const std::string &label) {
            MetadataRestoreProposal proposal;
            proposal.stickTrack = stickTrack(title);
            proposal.storedFrom = label;
            proposal.storedFromLibraryId = libraryId;
            proposal.cuesOffered = true;
            return proposal;
        };
        const auto inScopeOf = [](const std::vector<MetadataRestoreProposal> &proposals, const std::string &key) {
            int n = 0;
            for (const auto &proposal : proposals) {
                n += proposalInRestoreScope(proposal, MetadataRestoreScope{key, {}}) ? 1 : 0;
            }
            return n;
        };

        // Partial stamping: RV2's three rows, one of them from before ids.
        {
            std::vector<MetadataRestoreProposal> proposals = {
                from("A", "uuid-rv2", "RV2"), from("B", "", "RV2"), from("C", "uuid-rv2", "RV2"),
                from("D", "", "LONELY"), from("E", "", ""),
            };
            resolveRestoreSources(proposals);
            assert(restoreSourceKey(proposals[1]) == restoreSourceKey(proposals[0])
                   && "an unstamped row joins the one stick recorded under its label");
            const auto sources = restoreSources(proposals);
            assert(sources.size() == 3 && "RV2 is one entry, LONELY one, the unknown stick one");
            const auto rv2 = std::find_if(sources.begin(), sources.end(),
                                          [](const auto &source) { return source.label == "RV2"; });
            assert(rv2 != sources.end() && rv2->key == "id:uuid-rv2" && rv2->proposalCount == 3);
            assert(!rv2->idNotRecorded);
            assert(inScopeOf(proposals, rv2->key) == 3 && "picking RV2 restores the whole stick");
            assert(restoreSourceKey(proposals[3]) == "label:LONELY" && "a label no id was recorded for stays a label");
            assert(restoreSourceKey(proposals[4]) == "unknown");
            for (const auto &source : sources) {
                assert(!source.idNotRecorded && "nothing here is ambiguous");
            }
        }

        // Two recorded sticks share the label: an unstamped row cannot be
        // put under either, and says so.
        {
            std::vector<MetadataRestoreProposal> proposals = {
                from("A", "uuid-1", "NO NAME"), from("B", "uuid-2", "NO NAME"), from("C", "", "NO NAME"),
                from("D", "", "NO NAME"),
            };
            resolveRestoreSources(proposals);
            assert(restoreSourceKey(proposals[0]) != restoreSourceKey(proposals[1]) && "two sticks stay two");
            assert(restoreSourceKey(proposals[2]) == "label:NO NAME" && "not guessed onto either stick");
            const auto sources = restoreSources(proposals);
            assert(sources.size() == 3);
            int unrecorded = 0;
            for (const auto &source : sources) {
                assert(source.label == "NO NAME");
                if (source.idNotRecorded) {
                    unrecorded++;
                    assert(source.key == "label:NO NAME" && source.proposalCount == 2);
                }
            }
            assert(unrecorded == 1 && "the unstamped rows are marked as such");
            assert(inScopeOf(proposals, "id:uuid-1") == 1 && inScopeOf(proposals, "id:uuid-2") == 1);
            assert(inScopeOf(proposals, "label:NO NAME") == 2);
        }
        std::cout << "case 17 (a stick whose rows are only partly stamped with its id is one stick) OK\n";
    }

    std::cout << "all metadata_restore_test cases passed\n";
    return 0;
}
