// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <cassert>
#include <set>
#include <iostream>
#include <vector>

#include "infrastructure/cleanup/pending_deletion_resolver.hpp"

using namespace seabass::domain;
using namespace seabass::infrastructure::cleanup;
using seabass::application::CatalogTracks;

namespace
{

PendingDeletion makePending(std::string filePath, std::string title)
{
    PendingDeletion p;
    p.format = "rekordbox";
    p.filePath = std::move(filePath);
    p.title = std::move(title);
    p.artist = "Artist";
    p.backupId = "20260101T000000-duplicate-file-cleanup";
    return p;
}

Track makeTrack(std::string filePath)
{
    Track t;
    t.sourceId = "1";
    t.filePath = std::move(filePath);
    return t;
}

// The catalogs a stick with only rekordbox on it would present.
CatalogTracks rekordboxOnly(std::vector<Track> tracks)
{
    CatalogTracks catalogs;
    catalogs.rekordbox = std::move(tracks);
    return catalogs;
}

// Every entry that went in comes back exactly once, in exactly one
// bucket. Checked on every call rather than per case, because this is
// the property the whole function is: an entry that fell out of all
// three buckets is a file nobody will ever offer to delete again, and
// one counted twice is an entry acted on twice. A case that asserts
// "safeToDelete.size() == 1" says nothing about either.
//
// Titles are the key here: this test gives each entry its own, and a
// path may legitimately repeat.
//
// What this does NOT say is which bucket is the right one: the answer
// "still referenced" turning into "safe to delete" keeps every entry
// accounted for. That is what the cases either side of each call check,
// one situation at a time; this is the property none of them can see.
void everyEntryIsAccountedFor(const std::vector<PendingDeletion> &pending, const PendingDeletionResolution &result)
{
    std::multiset<std::string> before;
    for (const auto &entry : pending) {
        before.insert(entry.title + "|" + entry.filePath);
    }
    std::multiset<std::string> after;
    std::multiset<std::string> seenInABucket;
    for (const auto *bucket : {&result.safeToDelete, &result.stillReferenced, &result.notOnThisStick}) {
        std::multiset<std::string> thisBucket;
        for (const auto &entry : *bucket) {
            const std::string key = entry.title + "|" + entry.filePath;
            after.insert(key);
            thisBucket.insert(key);
        }
        for (const auto &key : thisBucket) {
            // In one bucket only. The union adding up is not enough on
            // its own: an entry copied into two buckets and another
            // dropped would leave the totals looking right, and "safe to
            // delete AND still referenced" is the answer that destroys a
            // file.
            assert(seenInABucket.count(key) == 0 && "an entry is in more than one bucket");
        }
        for (const auto &key : thisBucket) {
            seenInABucket.insert(key);
        }
    }
    if (before != after) {
        std::cerr << "the buckets do not add up: " << pending.size() << " in, " << after.size() << " out ("
                  << result.safeToDelete.size() << " to delete, " << result.stillReferenced.size()
                  << " still referenced, " << result.notOnThisStick.size() << " elsewhere)\n";
    }
    assert(before == after && "every pending entry lands in exactly one bucket");
}

// resolvePendingDeletions() with the invariant checked around it, so no
// case can forget.
PendingDeletionResolution resolve(const std::vector<PendingDeletion> &pending, const CatalogTracks &catalogs,
                                  const std::string &stickRoot)
{
    PendingDeletionResolution result = resolvePendingDeletions(pending, catalogs, stickRoot);
    everyEntryIsAccountedFor(pending, result);
    return result;
}

}  // namespace

int main()
{
    // A pending entry whose file is genuinely unreferenced by any current
    // track is safe to delete -- this is the exact bug-shaped scenario:
    // the file really is an orphaned duplicate.
    {
        std::vector<PendingDeletion> pending = {makePending("/stick/Contents/dup.mp3", "Duplicate Track")};
        std::vector<Track> current = {makeTrack("/stick/Contents/survivor.mp3")};

        auto result = resolve(pending, rekordboxOnly(current), "/stick");
        assert(result.safeToDelete.size() == 1);
        assert(result.safeToDelete[0].filePath == "/stick/Contents/dup.mp3");
        assert(result.stillReferenced.empty());
        std::cout << "case 1 (unreferenced file -> safe to delete) OK\n";
    }

    // A pending entry whose file path is STILL referenced by a current
    // track (the manifest is stale, or something re-pointed at it since)
    // must never be deleted, no matter what the manifest says.
    {
        std::vector<PendingDeletion> pending = {makePending("/stick/Contents/still-used.mp3", "Still Used")};
        std::vector<Track> current = {makeTrack("/stick/Contents/still-used.mp3")};

        auto result = resolve(pending, rekordboxOnly(current), "/stick");
        assert(result.safeToDelete.empty());
        assert(result.stillReferenced.size() == 1);
        assert(result.stillReferenced[0].filePath == "/stick/Contents/still-used.mp3");
        std::cout << "case 2 (still-referenced file -> left alone, reported) OK\n";
    }

    // Path-separator differences between how the manifest recorded a
    // path and how a fresh scan reports it still match -- Windows paths
    // round-trip through both styles depending on which code path
    // produced them.
    {
        std::vector<PendingDeletion> pending = {makePending("C:\\Stick\\Contents\\dup.mp3", "Dup")};
        std::vector<Track> current = {makeTrack("C:/Stick/Contents/dup.mp3")};

        auto result = resolve(pending, rekordboxOnly(current), "C:/Stick");
        assert(result.safeToDelete.empty());
        assert(result.stillReferenced.size() == 1);
        std::cout << "case 3 (path-separator-insensitive matching) OK\n";
    }

    // An entry with no resolved file path at all is left alone rather
    // than guessed at -- nothing to safely verify against.
    {
        std::vector<PendingDeletion> pending = {makePending("", "No Path")};
        std::vector<Track> current;

        auto result = resolve(pending, rekordboxOnly(current), "/stick");
        assert(result.safeToDelete.empty());
        assert(result.stillReferenced.size() == 1);
        std::cout << "case 4 (empty filePath -> left alone, never guessed at) OK\n";
    }

    // A mixed batch classifies each entry independently.
    {
        std::vector<PendingDeletion> pending = {
            makePending("/stick/Contents/orphan-a.mp3", "Orphan A"),
            makePending("/stick/Contents/used.mp3", "Used"),
            makePending("/stick/Contents/orphan-b.mp3", "Orphan B"),
        };
        std::vector<Track> current = {makeTrack("/stick/Contents/used.mp3")};

        auto result = resolve(pending, rekordboxOnly(current), "/stick");
        assert(result.safeToDelete.size() == 2);
        assert(result.stillReferenced.size() == 1);
        assert(result.stillReferenced[0].filePath == "/stick/Contents/used.mp3");
        std::cout << "case 5 (mixed batch classified independently) OK\n";
    }

    // The bug this signature exists to make impossible: a cleanup in one
    // catalog orphans a file, but ANOTHER catalog on the same stick still
    // references it. Checking only the format that created the entry
    // would permanently destroy a file the DJ can still play.
    {
        std::vector<PendingDeletion> pending = {makePending("/stick/Contents/shared.mp3", "Shared Track")};

        // Only rekordbox consulted -- and rekordbox has indeed forgotten it.
        auto oneCatalog = resolve(pending, rekordboxOnly({makeTrack("/stick/Contents/other.mp3")}), "/stick");
        assert(oneCatalog.safeToDelete.size() == 1);  // this is what used to happen

        // Every catalog consulted: Engine still plays it, OneLibrary too.
        CatalogTracks all;
        all.rekordbox = {makeTrack("/stick/Contents/other.mp3")};
        all.engine = {makeTrack("/stick/Contents/shared.mp3")};
        all.oneLibrary = {makeTrack("/stick/Contents/shared.mp3")};

        auto result = resolve(pending, all, "/stick");
        assert(result.safeToDelete.empty());
        assert(result.stillReferenced.size() == 1);
        std::cout << "case 6 (a file another catalog still references is never deleted) OK\n";
    }

    // OneLibrary alone is enough to protect a file. It is the same
    // rekordbox library as export.pdb in a newer format, and the two do
    // NOT agree on contents -- on RV2, OneLibrary referenced 290 files
    // that export.pdb did not -- so this is the common case, not an
    // exotic one.
    {
        std::vector<PendingDeletion> pending = {makePending("/stick/Contents/kept.mp3", "Kept Track")};
        CatalogTracks catalogs;
        catalogs.rekordbox = std::vector<Track>{};
        catalogs.engine = std::vector<Track>{};
        catalogs.oneLibrary = {makeTrack("/stick/Contents/kept.mp3")};

        auto result = resolve(pending, catalogs, "/stick");
        assert(result.safeToDelete.empty());
        assert(result.stillReferenced.size() == 1);
        std::cout << "case 7 (OneLibrary alone protects a file) OK\n";
    }

    // No catalog could be read at all: protect everything. "I could not
    // look" and "nothing needs these" must never be the same answer.
    {
        std::vector<PendingDeletion> pending = {makePending("/stick/Contents/a.mp3", "A"),
                                                 makePending("/stick/Contents/b.mp3", "B")};
        auto result = resolve(pending, CatalogTracks{}, "/stick");
        assert(result.safeToDelete.empty());
        assert(result.stillReferenced.size() == 2);
        std::cout << "case 8 (no catalogs -> nothing is safe to delete) OK\n";
    }

    // Case-insensitive filesystems: exFAT and NTFS treat these as one
    // file, so a case-different spelling in the catalog still protects it.
    {
        std::vector<PendingDeletion> pending = {makePending("/stick/Contents/Artist/Track.mp3", "T")};
        auto result = resolve(pending, rekordboxOnly({makeTrack("/stick/contents/artist/TRACK.MP3")}), "/stick");
        assert(result.safeToDelete.empty());
        std::cout << "case 9 (case-different spelling still protects) OK\n";
    }

    // An entry pointing outside this stick's root: a moved mount point
    // (the clone now sits where the original was). Neither safe to
    // delete nor merely "still referenced": it is not this stick's file.
    {
        std::vector<PendingDeletion> pending = {makePending("/media/other/Contents/x.mp3", "Elsewhere"),
                                               makePending("/stick/Contents/dup.mp3", "Here")};
        std::vector<Track> current = {makeTrack("/stick/Contents/survivor.mp3")};
        auto result = resolve(pending, rekordboxOnly(current), "/stick");
        assert(result.notOnThisStick.size() == 1);
        assert(result.notOnThisStick[0].filePath == "/media/other/Contents/x.mp3");
        assert(result.safeToDelete.size() == 1 && result.safeToDelete[0].filePath == "/stick/Contents/dup.mp3");
        assert(isUnderStickRoot("/stick/Contents/a.mp3", "/stick/"));
        assert(isUnderStickRoot("/STICK/contents/a.mp3", "/stick"));
        assert(!isUnderStickRoot("/stick2/Contents/a.mp3", "/stick"));
        assert(!isUnderStickRoot("/stick", "/stick"));
        std::cout << "case: an entry outside the stick root is set aside, not deleted OK\n";
    }

    std::cout << "all cases passed\n";
    return 0;
}
