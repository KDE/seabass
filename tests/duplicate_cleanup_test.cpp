// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>

#include "domain/duplicate_cleanup.hpp"

using namespace seabass::domain;

namespace
{

Track makeTrack(std::string id, double duration, int bitrate, std::uint64_t sizeBytes, std::vector<CuePoint> cues = {})
{
    Track t;
    t.sourceId = std::move(id);
    t.filename = "song.mp3";
    t.durationSeconds = duration;
    t.bitrate = bitrate;
    t.fileSizeBytes = sizeBytes;
    t.cues = std::move(cues);
    return t;
}

// An audio file on the stick that no catalog references: no row, so no
// rating/comment/cues, and its title/artist/duration/bitrate come off
// the file itself.
Track makeStray(std::string id, double duration, int bitrate, std::uint64_t sizeBytes)
{
    Track t;
    t.sourceId = std::move(id);
    t.format = "disk";
    t.isUnreferenced = true;
    t.filename = "song.mp3";
    t.filePath = "/stick/Contents/song.mp3";
    t.durationSeconds = duration;
    t.bitrate = bitrate;
    t.fileSizeBytes = sizeBytes;
    return t;
}

bool hasTrack(const std::vector<Track> &tracks, const std::string &id)
{
    for (const auto &t : tracks) {
        if (t.sourceId == id) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main()
{
    // Higher bitrate, same duration -> that copy survives, no disagreement.
    {
        DuplicateGroup group{{makeTrack("a", 200.0, 128, 3'200'000), makeTrack("b", 200.0, 320, 8'000'000)}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.survivor.sourceId == "b");
        assert(plan.toRemove.size() == 1 && plan.toRemove[0].sourceId == "a");
        assert(!plan.differs);
        std::cout << "case 1 (higher bitrate survives, agree) OK\n";
    }

    // Higher-bitrate copy is meaningfully shorter -- quality and length
    // disagree on which is "best". Survivor is still the higher-bitrate
    // copy, but flagged for review rather than silently auto-applied.
    {
        DuplicateGroup group{{makeTrack("a", 200.0, 320, 8'000'000), makeTrack("b", 260.0, 128, 4'000'000)}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.survivor.sourceId == "a");
        assert(plan.differs);
        std::cout << "case 2 (quality vs length disagree -> differs=true) OK\n";
    }

    // Bitrate unknown on both -- falls back to duration, never flagged
    // as "differs" since there's no real quality signal to disagree with.
    {
        DuplicateGroup group{{makeTrack("a", 200.0, 0, 3'000'000), makeTrack("b", 260.0, 0, 3'500'000)}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.survivor.sourceId == "b");
        assert(!plan.differs);
        std::cout << "case 3 (bitrate unknown -> falls back to duration, never differs) OK\n";
    }

    // Cue merging: the survivor's own cues are kept as-is; a hot cue in
    // a different slot and a memory cue far from any existing one are
    // added from the removed copy; a colliding hot cue slot and a
    // near-duplicate memory cue are NOT duplicated.
    {
        std::vector<CuePoint> survivorCues = {
            CuePoint{CuePoint::Kind::Hot, 1, 1000.0, "#FF0000", "drop"},
            CuePoint{CuePoint::Kind::Memory, 0, 5000.0, "", ""},
        };
        std::vector<CuePoint> removedCues = {
            CuePoint{CuePoint::Kind::Hot, 1, 1500.0, "#00FF00", "different position, same slot"},  // slot taken, dropped
            CuePoint{CuePoint::Kind::Hot, 2, 2000.0, "#0000FF", "new slot"},                        // added
            CuePoint{CuePoint::Kind::Memory, 0, 5100.0, "", ""},                                    // near-duplicate, dropped
            CuePoint{CuePoint::Kind::Memory, 0, 40000.0, "", ""},                                   // far away, added
        };
        DuplicateGroup group{{makeTrack("a", 200.0, 320, 8'000'000, survivorCues),
                               makeTrack("b", 200.0, 128, 3'000'000, removedCues)}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.survivor.sourceId == "a");
        assert(plan.mergedCuesForSurvivor.size() == 4);  // 2 original + hot slot 2 + far memory cue
        std::cout << "case 4 (cue merge: adds gaps, never duplicates) OK\n";
    }

    // Degenerate single-track group -- survivor is that track, nothing
    // to remove.
    {
        DuplicateGroup group{{makeTrack("solo", 200.0, 320, 8'000'000)}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.survivor.sourceId == "solo");
        assert(plan.toRemove.empty());
        assert(!plan.differs);
        std::cout << "case 5 (single-track group -- nothing to remove) OK\n";
    }

    // Tie on bitrate -- longer duration wins the tie-break.
    {
        DuplicateGroup group{{makeTrack("a", 200.0, 320, 8'000'000), makeTrack("b", 210.0, 320, 8'100'000)}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.survivor.sourceId == "b");
        assert(hasTrack(plan.toRemove, "a"));
        std::cout << "case 6 (tied bitrate -> longer duration wins tie-break) OK\n";
    }

    // Propagation is a per-field "fill a gap", not a merge: survivor
    // ("a", higher bitrate) is missing bpm/key/artwork entirely; the
    // removed copy ("b") has all three. Each should carry forward, with
    // "b" recorded as the donor for each.
    {
        Track a = makeTrack("a", 200.0, 320, 8'000'000);
        Track b = makeTrack("b", 200.0, 128, 3'000'000);
        b.bpm = 128.0;
        b.key = "Fm";
        b.artworkPath = "/stick/art/b.jpg";
        DuplicateGroup group{{a, b}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.survivor.sourceId == "a");
        assert(plan.bpmForSurvivor.has_value() && *plan.bpmForSurvivor == 128.0);
        assert(plan.bpmDonorSourceId == "b");
        assert(plan.keyForSurvivor.has_value() && *plan.keyForSurvivor == "Fm");
        assert(plan.keyDonorSourceId == "b");
        assert(plan.artworkPathForSurvivor.has_value() && *plan.artworkPathForSurvivor == "/stick/art/b.jpg");
        assert(plan.artworkDonorSourceId == "b");
        std::cout << "case 7 (bpm/key/artwork propagate from donor when survivor lacks them) OK\n";
    }

    // Survivor already has bpm/key/artwork -- nothing propagates, even
    // though another copy also has values (there's no gap to fill).
    {
        Track a = makeTrack("a", 200.0, 320, 8'000'000);
        a.bpm = 174.0;
        a.key = "Am";
        a.artworkPath = "/stick/art/a.jpg";
        Track b = makeTrack("b", 200.0, 128, 3'000'000);
        b.bpm = 128.0;
        b.key = "Fm";
        b.artworkPath = "/stick/art/b.jpg";
        DuplicateGroup group{{a, b}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.survivor.sourceId == "a");
        assert(!plan.bpmForSurvivor.has_value());
        assert(!plan.keyForSurvivor.has_value());
        assert(!plan.artworkPathForSurvivor.has_value());
        std::cout << "case 8 (survivor already has bpm/key/artwork -- nothing propagates) OK\n";
    }

    // Neither copy has bpm/key/artwork -- nothing to propagate, no crash.
    {
        DuplicateGroup group{{makeTrack("a", 200.0, 320, 8'000'000), makeTrack("b", 200.0, 128, 3'000'000)}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(!plan.bpmForSurvivor.has_value());
        assert(!plan.keyForSurvivor.has_value());
        assert(!plan.artworkPathForSurvivor.has_value());
        std::cout << "case 9 (neither copy has bpm/key/artwork -- nothing propagates) OK\n";
    }

    // hasUnpreservableDataAtRisk: two copies with genuinely different
    // ratings -- real data that would be silently lost, distinct from
    // (and independent of) `differs`.
    {
        Track a = makeTrack("a", 200.0, 320, 8'000'000);
        a.rating = 5;
        Track b = makeTrack("b", 200.0, 128, 3'000'000);
        b.rating = 2;
        DuplicateGroup group{{a, b}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.hasUnpreservableDataAtRisk);
        assert(!plan.differs);
        std::cout << "case 10 (differing ratings -> hasUnpreservableDataAtRisk, not differs) OK\n";
    }

    // Only one copy has a rating (the other has none set) -- nothing to
    // lose, since there's only ever one real value in the group.
    {
        Track a = makeTrack("a", 200.0, 320, 8'000'000);
        a.rating = 5;
        Track b = makeTrack("b", 200.0, 128, 3'000'000);
        DuplicateGroup group{{a, b}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(!plan.hasUnpreservableDataAtRisk);
        std::cout << "case 11 (only one copy has a rating -- not at risk) OK\n";
    }

    // Both copies agree on rating/comment -- not at risk either.
    {
        Track a = makeTrack("a", 200.0, 320, 8'000'000);
        a.rating = 4;
        a.comment = "banger";
        Track b = makeTrack("b", 200.0, 128, 3'000'000);
        b.rating = 4;
        b.comment = "banger";
        DuplicateGroup group{{a, b}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(!plan.hasUnpreservableDataAtRisk);
        std::cout << "case 12 (agreeing rating/comment -- not at risk) OK\n";
    }

    // A differing comment trips the flag too, not just rating.
    {
        Track a = makeTrack("a", 200.0, 320, 8'000'000);
        a.comment = "keeper";
        Track b = makeTrack("b", 200.0, 128, 3'000'000);
        b.comment = "meh";
        DuplicateGroup group{{a, b}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.hasUnpreservableDataAtRisk);
        std::cout << "case 13 (differing comment -> hasUnpreservableDataAtRisk) OK\n";
    }
    // Play counts and last-played timestamps do NOT trip it, on purpose.
    // They are per-application counters: rekordbox keeps a running count,
    // Engine keeps only the timestamp of the last play, and the same
    // track routinely carries both. Comparing them across library types
    // asks a question with no answer, and treating the non-answer as
    // "data at risk" held back 57 audio files on a real stick where 2 was
    // the honest number. See duplicate_cleanup.cpp's own comment.
    {
        Track a = makeTrack("a", 200.0, 320, 8'000'000);
        a.playCount = 10;
        Track b = makeTrack("b", 200.0, 128, 3'000'000);
        b.playCount = 3;
        DuplicateGroup group{{a, b}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(!plan.hasUnpreservableDataAtRisk);
        std::cout << "case 14 (differing playCount alone does NOT flag) OK\n";
    }
    {
        Track a = makeTrack("a", 200.0, 320, 8'000'000);
        a.lastPlayedAt = std::chrono::system_clock::time_point{std::chrono::seconds{1000}};
        Track b = makeTrack("b", 200.0, 128, 3'000'000);
        b.lastPlayedAt = std::chrono::system_clock::time_point{std::chrono::seconds{2000}};
        DuplicateGroup group{{a, b}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(!plan.hasUnpreservableDataAtRisk);
        std::cout << "case 15 (differing lastPlayedAt alone does NOT flag) OK\n";
    }
    // ...but a rating still does, even alongside differing play counts:
    // dropping the counters must not have dropped the real signal too.
    {
        Track a = makeTrack("a", 200.0, 320, 8'000'000);
        a.rating = 5;
        a.playCount = 10;
        Track b = makeTrack("b", 200.0, 128, 3'000'000);
        b.rating = 2;
        b.playCount = 3;
        DuplicateGroup group{{a, b}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.hasUnpreservableDataAtRisk);
        std::cout << "case 15b (rating still flags alongside play counts) OK\n";
    }

    // `differs` (quality/length disagreement) and
    // `hasUnpreservableDataAtRisk` (per-copy DJ data disagreement) are
    // genuinely independent flags: this group differs on quality/length
    // but agrees on every DJ-data field, so only `differs` should be set.
    {
        Track a = makeTrack("a", 200.0, 320, 8'000'000);  // higher bitrate, shorter
        a.rating = 4;
        Track b = makeTrack("b", 260.0, 128, 4'000'000);  // lower bitrate, longer
        b.rating = 4;
        DuplicateGroup group{{a, b}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.differs);
        assert(!plan.hasUnpreservableDataAtRisk);
        std::cout << "case 16 (differs without hasUnpreservableDataAtRisk -- flags are independent) OK\n";
    }

    // Regression test for a real bug found in review: case 11 above
    // ("only one copy has a rating -- not at risk") happens to put the
    // rated track on the higher-bitrate copy, which is also the
    // survivor -- so it can't tell "only the survivor has it" (safe)
    // apart from "only a copy about to be REMOVED has it" (a real,
    // silent loss). Here the LOWER-bitrate (doomed) copy is the one
    // with the rating; the higher-bitrate (survivor) copy has none.
    // Must be flagged: cleanup would keep the unrated survivor and
    // discard the only rating that ever existed for this track.
    {
        Track a = makeTrack("a", 200.0, 320, 8'000'000);  // higher bitrate -> survivor, no rating
        Track b = makeTrack("b", 200.0, 128, 3'000'000);  // lower bitrate -> doomed, has a rating
        b.rating = 5;
        DuplicateGroup group{{a, b}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.survivor.sourceId == "a");
        assert(plan.hasUnpreservableDataAtRisk);
        std::cout << "case 17 (only a DOOMED copy has a rating -- flagged as at risk, not silently kept) OK\n";
    }

    // --- unreferenced files ------------------------------------------
    //
    // A stray file may not be the survivor while any catalogued copy is
    // in the group, even when it is the better copy by every normal
    // rule. Keeping it would leave the catalog pointing at the file we
    // then delete; repointing the row at the survivor is a different and
    // much larger feature.
    {
        Track catalogued = makeTrack("row", 200.0, 128, 3'000'000);
        Track stray = makeStray("/stick/Contents/song.mp3", 200.0, 320, 8'000'000);
        DuplicateGroup group{{catalogued, stray}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.survivor.sourceId == "row");
        assert(plan.unreferencedFilesToDelete.size() == 1);
        assert(plan.unreferencedFilesToDelete[0].sourceId == "/stick/Contents/song.mp3");
        assert(plan.unreferencedFilesHeldBack.empty());
        // The commonest shape on a real stick, and the reason applying
        // it needs no write session at all: the only removal is a file,
        // and a stray carries nothing to propagate onto the survivor.
        // cleanup_controller's writesToCatalog() reads exactly these
        // three facts, so if the planner ever starts propagating from a
        // stray, this is what says so.
        assert(plan.mergedCuesForSurvivor.size() == plan.survivor.cues.size());
        assert(!plan.bpmForSurvivor && !plan.keyForSurvivor && !plan.artworkPathForSurvivor);
        assert(std::none_of(plan.toRemove.begin(), plan.toRemove.end(),
                             [](const Track &t) { return !t.isUnreferenced; }));
        std::cout << "case 18 (a catalogued copy outranks a better stray file; nothing to write) OK\n";
    }

    // ...but a group of only stray files is a real case -- several
    // copies of a track that fell out of every catalog. Collapse it to
    // the best one; what survives is a re-import candidate, not a
    // deletion candidate, and is not listed for deletion.
    {
        DuplicateGroup group{{makeStray("/stick/Contents/a.mp3", 200.0, 128, 3'000'000),
                               makeStray("/stick/Contents/b.mp3", 200.0, 320, 8'000'000)}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.survivor.sourceId == "/stick/Contents/b.mp3");
        assert(plan.unreferencedFilesToDelete.size() == 1);
        assert(plan.unreferencedFilesToDelete[0].sourceId == "/stick/Contents/a.mp3");
        std::cout << "case 19 (an all-stray group collapses to its best copy) OK\n";
    }

    // A catalogued copy that is not the survivor loses a database row,
    // not a file -- it must never appear in either stray list.
    {
        DuplicateGroup group{{makeTrack("a", 200.0, 320, 8'000'000), makeTrack("b", 200.0, 128, 3'000'000)}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.toRemove.size() == 1);
        assert(plan.unreferencedFilesToDelete.empty());
        assert(plan.unreferencedFilesHeldBack.empty());
        std::cout << "case 20 (catalogued removals are rows, never file deletions) OK\n";
    }

    // An estimated duration holds back every stray in the group, not
    // just the file whose length was guessed: the group was formed on
    // that guess, so it is the grouping that is in doubt. Here the
    // estimate is on the CATALOGUED copy and the stray is exact --
    // still held back, because the two may not be the same track.
    {
        Track catalogued = makeTrack("row", 200.0, 128, 3'000'000);
        catalogued.durationIsEstimated = true;
        Track stray = makeStray("/stick/Contents/song.mp3", 201.0, 128, 3'000'000);
        DuplicateGroup group{{catalogued, stray}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.survivor.sourceId == "row");
        assert(plan.unreferencedFilesToDelete.empty());
        assert(plan.unreferencedFilesHeldBack.size() == 1);
        std::cout << "case 21 (an estimated duration anywhere in the group holds every stray back) OK\n";
    }

    // `differs` -- quality and length disagree, which is how a
    // deliberately different edit shows up -- holds strays back too:
    // the file may not be a copy of this track at all.
    {
        Track catalogued = makeTrack("row", 200.0, 320, 8'000'000);
        Track stray = makeStray("/stick/Contents/song.mp3", 260.0, 128, 4'000'000);
        DuplicateGroup group{{catalogued, stray}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.differs);
        assert(plan.unreferencedFilesToDelete.empty());
        assert(plan.unreferencedFilesHeldBack.size() == 1);
        std::cout << "case 22 (a `differs` group holds its strays back) OK\n";
    }

    // The decision this feature turned on: hasUnpreservableDataAtRisk
    // does NOT hold a stray file back. Two catalogued rows disagree on a
    // rating, which is real and holds the row-level cleanup back -- but
    // the stray has no row and carries no rating, so deleting the file
    // loses nothing the flag protects. On a real stick this coupling
    // held back 2 of 632 stray files over a disagreement neither was
    // party to (57 of them before play counts stopped counting).
    {
        Track a = makeTrack("a", 200.0, 320, 8'000'000);
        a.rating = 5;
        Track b = makeTrack("b", 200.0, 128, 3'000'000);
        b.rating = 2;
        Track stray = makeStray("/stick/Contents/song.mp3", 200.0, 192, 4'000'000);
        DuplicateGroup group{{a, b, stray}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.hasUnpreservableDataAtRisk);
        assert(plan.survivor.sourceId == "a");
        assert(plan.unreferencedFilesToDelete.size() == 1);
        assert(plan.unreferencedFilesToDelete[0].sourceId == "/stick/Contents/song.mp3");
        assert(plan.unreferencedFilesHeldBack.empty());
        std::cout << "case 23 (data at risk on catalog rows does not hold a stray file back) OK\n";
    }

    // A stray can never be the reason for that flag: it has nowhere to
    // have stored a rating or a comment.
    {
        Track catalogued = makeTrack("row", 200.0, 320, 8'000'000);
        Track stray = makeStray("/stick/Contents/song.mp3", 200.0, 128, 3'000'000);
        DuplicateGroup group{{catalogued, stray}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(!plan.hasUnpreservableDataAtRisk);
        std::cout << "case 24 (a stray file never trips hasUnpreservableDataAtRisk) OK\n";
    }

    // --- one file, written in several formats -------------------------
    //
    // Removing a copy drops its row from every format that carries it,
    // and each of those removals repoints that format's playlists at the
    // surviving file -- which needs the survivor to be carried there
    // too. When it is, the group is ordinary.
    {
        Track keep = makeTrack("rb-1", 200.0, 320, 8'000'000);
        keep.filePath = "/stick/Contents/a.mp3";
        keep.catalogRows = {{"rekordbox", "rb-1"}, {"engine", "en-1"}, {"onelibrary", "ol-1"}};
        Track drop = makeTrack("rb-2", 200.0, 128, 3'000'000);
        drop.filePath = "/stick/Contents/a-1.mp3";
        drop.catalogRows = {{"rekordbox", "rb-2"}, {"engine", "en-2"}};
        DuplicateGroup group{{keep, drop}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.survivor.sourceId == "rb-1");
        assert(!plan.wouldStrandAFormat);
        assert(plan.toRemove.size() == 1 && plan.toRemove[0].catalogRows.size() == 2);
        std::cout << "case 25 (a copy carries every format's row that must go with it) OK\n";
    }

    // But when the doomed copy is written somewhere the survivor is not,
    // the formats have already diverged, and dropping that row would
    // take the recording out of that format altogether -- leaving them
    // further apart. Nothing here can repoint it, so the group is held,
    // not merely unchecked.
    {
        Track keep = makeTrack("rb-1", 200.0, 320, 8'000'000);
        keep.filePath = "/stick/Contents/a.mp3";
        keep.catalogRows = {{"rekordbox", "rb-1"}};
        Track drop = makeTrack("en-2", 200.0, 128, 3'000'000);
        drop.filePath = "/stick/Contents/a-1.mp3";
        drop.catalogRows = {{"engine", "en-2"}};
        DuplicateGroup group{{keep, drop}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(plan.survivor.sourceId == "rb-1");
        assert(plan.wouldStrandAFormat);
        std::cout << "case 26 (a removal that would strand a format is held) OK\n";
    }

    // A caller working one format at a time sets no catalogRows at all,
    // and must see exactly the behaviour it always did.
    {
        DuplicateGroup group{{makeTrack("a", 200.0, 320, 8'000'000), makeTrack("b", 200.0, 128, 3'000'000)}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(!plan.wouldStrandAFormat);
        std::cout << "case 27 (no catalogRows -- the rule cannot fire) OK\n";
    }

    // catalogsWrittenBy(): which catalogs a plan's removals actually
    // live in. This is what lets a writer refuse a plan it can only
    // partly apply, so it has to be exact about the two edge shapes --
    // a stray file (no catalog at all) and a collapsed file (several).
    {
        // Uncollapsed: each row is its own track, one catalog.
        DuplicateGroup group{{makeTrack("a", 200.0, 320, 3'000'000), makeTrack("b", 200.0, 256, 2'500'000)}};
        group.tracks[0].format = "engine";
        group.tracks[1].format = "engine";
        auto plan = DuplicateCleanupPlanner::plan(group);
        auto catalogs = catalogsWrittenBy(plan);
        assert(catalogs.size() == 1);
        assert(catalogs[0] == "engine");
        std::cout << "case 20 (uncollapsed plan writes one catalog) OK\n";
    }
    {
        // Collapsed: the doomed copy is one file with rows in three
        // catalogs, and removing it means removing all three.
        DuplicateGroup group{{makeTrack("keep", 200.0, 320, 3'000'000), makeTrack("drop", 200.0, 256, 2'500'000)}};
        group.tracks[0].format = "rekordbox";
        group.tracks[1].format = "rekordbox";
        group.tracks[1].catalogRows = {{"rekordbox", "drop"}, {"engine", "e7"}, {"onelibrary", "o9"}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        auto catalogs = catalogsWrittenBy(plan);
        assert(catalogs.size() == 3);
        assert(catalogs[0] == "rekordbox");
        assert(std::find(catalogs.begin(), catalogs.end(), "engine") != catalogs.end());
        assert(std::find(catalogs.begin(), catalogs.end(), "onelibrary") != catalogs.end());
        std::cout << "case 21 (a collapsed file names every catalog it is in) OK\n";
    }
    {
        // A stray file is in no catalog, so removing it writes to none.
        DuplicateGroup group{{makeTrack("keep", 200.0, 320, 3'000'000), makeTrack("stray", 200.0, 256, 2'500'000)}};
        group.tracks[0].format = "engine";
        group.tracks[1].format = "engine";
        group.tracks[1].isUnreferenced = true;
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(catalogsWrittenBy(plan).empty());
        std::cout << "case 22 (a stray file writes to no catalog) OK\n";
    }

    // --- writeTargetsFor: the per-format ids a multi-catalog save needs ---

    // The ordinary uncollapsed case. Every row is its own track, so the
    // one format in play answers with its own ids and every other format
    // answers with nothing at all -- not with the ids it does not own.
    {
        DuplicateGroup group{{makeTrack("a", 200.0, 320, 8'000'000), makeTrack("b", 200.0, 128, 3'000'000)}};
        group.tracks[0].format = "rekordbox";
        group.tracks[1].format = "rekordbox";
        auto plan = DuplicateCleanupPlanner::plan(group);

        auto rekordbox = writeTargetsFor(plan, "rekordbox");
        assert(rekordbox.survivorSourceId == plan.survivor.sourceId);
        assert(rekordbox.doomedSourceIds.size() == 1);
        assert(rekordbox.doomedSourceIds[0] == plan.toRemove[0].sourceId);
        assert(canWriteWholeCatalog(rekordbox));
        assert(!hasNoWork(rekordbox));

        auto engine = writeTargetsFor(plan, "engine");
        assert(engine.survivorSourceId.empty());
        assert(engine.doomedSourceIds.empty());
        assert(hasNoWork(engine));
        // Nothing to do is not the same as unsafe: a catalog with no rows
        // in this plan is simply not this plan's business.
        assert(canWriteWholeCatalog(engine));
        std::cout << "case 23 (an uncollapsed plan answers for its own format only) OK\n";
    }

    // A collapsed file: one file, a row in each catalog, different ids.
    // Handing rekordbox's id to Engine's writer is the bug this exists to
    // prevent, so each format must answer with its own.
    {
        DuplicateGroup group{{makeTrack("a", 200.0, 320, 8'000'000), makeTrack("b", 200.0, 128, 3'000'000)}};
        group.tracks[0].format = "rekordbox";
        group.tracks[0].catalogRows = {{"rekordbox", "rb-keep"}, {"engine", "en-keep"}};
        group.tracks[1].format = "rekordbox";
        group.tracks[1].catalogRows = {{"rekordbox", "rb-drop"}, {"engine", "en-drop"}};
        auto plan = DuplicateCleanupPlanner::plan(group);

        auto rekordbox = writeTargetsFor(plan, "rekordbox");
        assert(rekordbox.survivorSourceId == "rb-keep");
        assert(rekordbox.doomedSourceIds == std::vector<std::string>{"rb-drop"});

        auto engine = writeTargetsFor(plan, "engine");
        assert(engine.survivorSourceId == "en-keep");
        assert(engine.doomedSourceIds == std::vector<std::string>{"en-drop"});

        // The ids must actually differ per format, or this test would
        // pass just as well against a writer that used one id everywhere.
        assert(rekordbox.survivorSourceId != engine.survivorSourceId);
        assert(rekordbox.doomedSourceIds[0] != engine.doomedSourceIds[0]);
        assert(canWriteWholeCatalog(rekordbox) && canWriteWholeCatalog(engine));
        std::cout << "case 24 (a collapsed file answers with each catalog's own ids) OK\n";
    }

    // The unsafe shape: the doomed copy has an Engine row, the survivor
    // does not. Engine cannot be written -- there is no id to repoint its
    // playlists at -- and the planner must already have refused the whole
    // plan. Asserting both together is the point: the per-format check
    // and the plan-level flag are two views of one condition, and this is
    // what stops them drifting apart.
    {
        DuplicateGroup group{{makeTrack("a", 200.0, 320, 8'000'000), makeTrack("b", 200.0, 128, 3'000'000)}};
        group.tracks[0].format = "rekordbox";
        group.tracks[0].catalogRows = {{"rekordbox", "rb-keep"}};
        group.tracks[1].format = "rekordbox";
        group.tracks[1].catalogRows = {{"rekordbox", "rb-drop"}, {"engine", "en-drop"}};
        auto plan = DuplicateCleanupPlanner::plan(group);

        auto engine = writeTargetsFor(plan, "engine");
        assert(engine.survivorSourceId.empty());
        assert(engine.doomedSourceIds == std::vector<std::string>{"en-drop"});
        assert(!canWriteWholeCatalog(engine));
        assert(!hasNoWork(engine));  // there IS work; it just cannot be done

        assert(plan.wouldStrandAFormat);
        // rekordbox itself is perfectly writable -- which is exactly why
        // the refusal has to be per-plan and not per-catalog: writing the
        // half that works is what splits the library.
        assert(canWriteWholeCatalog(writeTargetsFor(plan, "rekordbox")));
        std::cout << "case 25 (a catalog the survivor is missing from cannot be written, and the plan says so) OK\n";
    }

    // The two views agree in the safe direction too, not only the unsafe
    // one -- otherwise case 25 would pass against a flag that was simply
    // always true.
    {
        DuplicateGroup group{{makeTrack("a", 200.0, 320, 8'000'000), makeTrack("b", 200.0, 128, 3'000'000)}};
        group.tracks[0].catalogRows = {{"rekordbox", "rb-keep"}, {"engine", "en-keep"}};
        group.tracks[1].catalogRows = {{"rekordbox", "rb-drop"}, {"engine", "en-drop"}};
        auto plan = DuplicateCleanupPlanner::plan(group);
        assert(!plan.wouldStrandAFormat);
        for (const auto &format : catalogsWrittenBy(plan)) {
            assert(canWriteWholeCatalog(writeTargetsFor(plan, format)));
        }
        std::cout << "case 26 (a plan that strands nothing is writable in every catalog it touches) OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
