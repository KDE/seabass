#include <cassert>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <sqlite3.h>

#include "application/ports/progress_reporter.hpp"
#include "domain/track.hpp"
#include "infrastructure/local/metadata_store.hpp"

#include "scratch_path.hpp"

using seabass::application::CancellationToken;
using seabass::application::NullProgressReporter;
using seabass::domain::CuePoint;
using seabass::domain::PlaylistMembership;
using seabass::domain::Track;
using seabass::infrastructure::local::MetadataSource;
using seabass::infrastructure::local::MetadataStore;
namespace fs = std::filesystem;

namespace
{

fs::path scratchRoot()
{
    // seabass::testing::scratchRoot() (scratch_path.hpp) already
    // namespaces by pid -- ::getpid() doesn't exist in that unqualified
    // form on Windows, and appending it again here was redundant even on
    // platforms where it does.
    return seabass::testing::scratchRoot() / "seabass-metadata-store-test";
}

void writeFile(const fs::path &path, const std::string &data)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << data;
}

CuePoint memoryCue(double positionMs)
{
    CuePoint cue;
    cue.kind = CuePoint::Kind::Memory;
    cue.positionMs = positionMs;
    return cue;
}

CuePoint hotCue(int number, double positionMs, const std::string &color = "#FF0000")
{
    CuePoint cue;
    cue.kind = CuePoint::Kind::Hot;
    cue.hotCueNumber = number;
    cue.positionMs = positionMs;
    cue.color = color;
    return cue;
}

Track sampleTrack(const fs::path &stickRoot, const std::string &relative, const std::string &title)
{
    Track track;
    track.format = "rekordbox";
    track.sourceId = title;
    track.filePath = (stickRoot / relative).string();
    track.filename = fs::path(relative).filename().string();
    track.title = title;
    track.artist = "Kalte Nacht";
    track.durationSeconds = 361.5;
    track.bpm = 128.0;
    track.key = "Am";
    return track;
}

// Three catalog dates, and they are the only dates the merge rule ever
// compares. A stored row is dated with the catalog mtime of the stick
// its value came from, not with the clock, so both sides of every
// comparison below are one of these three. The wall clock does not
// enter into it, which is what makes these cases deterministic.
constexpr std::int64_t CatalogOld = 1'700'000'000;  // 2023
constexpr std::int64_t CatalogMid = 1'750'000'000;  // 2025
constexpr std::int64_t CatalogNew = 1'800'000'000;     // 2027
constexpr std::int64_t CatalogNewest = 1'850'000'000;  // 2028

MetadataSource sourceFor(const fs::path &stickRoot, const std::string &label = "RV2",
                          std::int64_t catalogModifiedAt = CatalogMid)
{
    MetadataSource source;
    source.stickRoot = stickRoot;
    source.libraryId = "library-abc";
    source.stickLabel = label;
    source.catalogModifiedAt = catalogModifiedAt;
    return source;
}

seabass::infrastructure::local::MetadataBackupSummary store(MetadataStore &store,
                                                             const std::vector<Track> &tracks,
                                                             const MetadataSource &source)
{
    return store.store(tracks, source, NullProgressReporter::instance(), CancellationToken::none());
}

}  // namespace

int main()
{
    const fs::path root = scratchRoot();
    fs::remove_all(root);
    const fs::path stick = root / "stick";
    const fs::path db = root / "local" / "metadata.db";

    // Two tracks that share one cover image, so the content-addressed
    // artwork store has something to collapse.
    const fs::path cover = stick / "PIONEER" / "Artwork" / "cover.jpg";
    writeFile(cover, "JPEGDATA-one-cover-for-two-tracks");
    writeFile(stick / "Contents" / "Kalte Nacht" / "Erste.mp3", "audio");
    writeFile(stick / "Contents" / "Kalte Nacht" / "Zweite.mp3", "audio");

    // ---- case 1: a first backup ------------------------------------
    {
        MetadataStore metadata(db);

        Track first = sampleTrack(stick, "Contents/Kalte Nacht/Erste.mp3", "Erste");
        first.cues = {memoryCue(0.0), hotCue(1, 32000.0)};
        first.rating = 4;
        first.comment = "opener";
        first.artworkPath = cover.string();
        first.playlists = {PlaylistMembership{"Techno/Peak Time", 3}};

        Track second = sampleTrack(stick, "Contents/Kalte Nacht/Zweite.mp3", "Zweite");
        second.artworkPath = cover.string();

        const auto summary = store(metadata, {first, second}, sourceFor(stick));
        assert(summary.tracksSeen == 2);
        assert(summary.tracksAdded == 2);
        assert(summary.tracksUpdated == 0);
        assert(summary.cuesStored == 2);
        // One image on disk for two tracks: the second track's cover
        // hashes to a file that is already there.
        assert(summary.artworkFilesAdded == 1);
        assert(metadata.trackCount() == 2);

        const auto rows = metadata.browse("", 50, 0);
        assert(rows.size() == 2);
        // Ordered by artist then title, so "Erste" leads "Zweite".
        assert(rows[0].title == "Erste");
        assert(rows[0].cueCount == 2);
        assert(rows[0].playlistCount == 1);
        assert(rows[0].rating.has_value() && *rows[0].rating == 4);
        assert(rows[0].stickLabel == "RV2");
        assert(!rows[0].artworkPath.empty() && fs::exists(rows[0].artworkPath));
        assert(rows[1].title == "Zweite");
        assert(rows[1].cueCount == 0);
        assert(!rows[1].rating.has_value());
        // Both tracks point at the same image file.
        assert(rows[0].artworkPath == rows[1].artworkPath);
        std::cout << "case 1 (first backup, cover art shared) OK\n";

        // readAll() has to hand the cover over too, not just browse().
        // It is the only side that knows where the copy landed, and a
        // restore proposal has no other way to draw one: the stick this
        // page exists for is the stick that lost its artwork. It used to
        // select every other field and leave this one empty, so every
        // proposal rendered a blank tile.
        const auto all = metadata.readAll();
        assert(all.size() == 2);
        for (const auto &track : all) {
            assert(!track.artworkPath.empty());
            assert(fs::exists(track.artworkPath));
            assert(track.artworkPath == rows[0].artworkPath);
        }
        std::cout << "case 1b (readAll hands over the cover it copied) OK\n";
    }

    // ---- case 2: the same stick again, nothing new ------------------
    {
        MetadataStore metadata(db);
        Track first = sampleTrack(stick, "Contents/Kalte Nacht/Erste.mp3", "Erste");
        first.cues = {memoryCue(0.0), hotCue(1, 32000.0)};
        first.rating = 4;
        first.comment = "opener";
        first.artworkPath = cover.string();

        const auto summary = store(metadata, {first}, sourceFor(stick));
        assert(summary.tracksAdded == 0);
        // Nothing was written, because nothing differed. Under the old
        // per-run policy an "overwrite" run rewrote every field it was
        // given whether or not it differed, which meant deleting and
        // re-inserting every cue of every track on every run. The merge
        // rule's first question is whether the two copies disagree at
        // all, and a run over an unchanged stick now answers no.
        assert(summary.tracksUpdated == 0);
        assert(summary.tracksUnchanged == 1);
        assert(summary.tracksSkipped == 0);
        assert(summary.cuesStored == 0);
        // The image was already stored, so nothing is hashed or copied
        // again -- a second run over an unchanged stick must not cost
        // what the first did.
        assert(summary.artworkFilesAdded == 0);
        assert(metadata.trackCount() == 2);
        // And the cues it did not rewrite are still there.
        assert(metadata.browse("Erste", 10, 0)[0].cueCount == 2);
        std::cout << "case 2 (an unchanged stick is read and nothing is rewritten) OK\n";
    }

    // ---- case 3: a disagreement the stored copy wins ----------------
    //
    // Same number of cues on each side, in different places, and a stick
    // whose catalogs have not been written since the entry was stored.
    // Nothing separates the two sets but their dates, so what is stored
    // stays. The empty incoming comment erases nothing either way.
    {
        MetadataStore metadata(db);
        Track changed = sampleTrack(stick, "Contents/Kalte Nacht/Erste.mp3", "Erste");
        changed.cues = {memoryCue(0.0), hotCue(1, 48000.0)};  // hot cue moved
        changed.rating = 2;
        changed.comment = "";  // nothing incoming: never a conflict

        const auto kept = store(metadata, {changed}, sourceFor(stick, "RV2", CatalogOld));
        assert(kept.tracksSkipped == 1);
        assert(kept.tracksUpdated == 0);
        auto rows = metadata.browse("Erste", 10, 0);
        assert(rows.size() == 1);
        assert(*rows[0].rating == 4);
        assert(rows[0].comment == "opener");  // untouched, not blanked
        auto cues = metadata.cuesFor(rows[0].id);
        assert(cues.size() == 2);
        assert(cues[1].positionMs == 32000.0);

        // ---- case 3b: the same disagreement, a stick worked on since
        //
        // The only thing that changed is which side was written last,
        // and that is enough to reverse every decision above.
        const auto taken = store(metadata, {changed}, sourceFor(stick, "RV2", CatalogNew));
        assert(taken.tracksUpdated == 1);
        assert(taken.tracksSkipped == 0);
        rows = metadata.browse("Erste", 10, 0);
        assert(*rows[0].rating == 2);
        assert(rows[0].comment == "opener");  // an empty incoming value erases nothing
        cues = metadata.cuesFor(rows[0].id);
        assert(cues.size() == 2);
        assert(cues[1].positionMs == 48000.0);
        std::cout << "case 3 (the later edit wins when nothing else separates them) OK\n";
    }

    // ---- case 3c: more cues beats a more recent edit -----------------
    //
    // The case the rule exists for. A re-export leaves one cue where
    // three used to be, and the catalog it leaves behind is brand new.
    // The larger stored set survives anyway.
    {
        const fs::path cueCountDb = root / "cue-counts" / "metadata.db";
        MetadataStore metadata(cueCountDb);
        Track full = sampleTrack(stick, "Contents/Kalte Nacht/Erste.mp3", "Erste");
        full.cues = {memoryCue(0.0), hotCue(1, 32000.0), hotCue(2, 64000.0)};
        store(metadata, {full}, sourceFor(stick, "RV2", CatalogOld));

        Track reExported = sampleTrack(stick, "Contents/Kalte Nacht/Erste.mp3", "Erste");
        reExported.cues = {memoryCue(0.0)};
        const auto summary = store(metadata, {reExported}, sourceFor(stick, "RV2", CatalogNew));
        assert(summary.tracksSkipped == 1);
        assert(summary.tracksUpdated == 0);
        const auto rows = metadata.browse("Erste", 10, 0);
        assert(rows[0].cueCount == 3);

        // And the other direction: a stick that has gained cues gives
        // them to a store that has fewer, however old the catalog is.
        Track recued = sampleTrack(stick, "Contents/Kalte Nacht/Erste.mp3", "Erste");
        recued.cues = {memoryCue(0.0), hotCue(1, 32000.0), hotCue(2, 64000.0), hotCue(3, 96000.0)};
        const auto grew = store(metadata, {recued}, sourceFor(stick, "RV2", CatalogOld));
        assert(grew.tracksUpdated == 1);
        assert(metadata.browse("Erste", 10, 0)[0].cueCount == 4);
        std::cout << "case 3c (more cues wins, whichever side was written last) OK\n";
    }

    // ---- case 4: filling a blank is not a conflict ------------------
    {
        MetadataStore metadata(db);
        Track second = sampleTrack(stick, "Contents/Kalte Nacht/Zweite.mp3", "Zweite");
        second.cues = {memoryCue(1000.0)};
        second.rating = 5;

        // A stick older than the stored row, so every dated decision
        // would go the other way. The stored row has neither cues nor a
        // rating, so there is nothing to keep and both land regardless.
        const auto summary = store(metadata, {second}, sourceFor(stick, "RV2", CatalogOld));
        assert(summary.tracksUpdated == 1);
        assert(summary.tracksSkipped == 0);
        const auto rows = metadata.browse("Zweite", 10, 0);
        assert(rows[0].cueCount == 1);
        assert(*rows[0].rating == 5);
        std::cout << "case 4 (a blank is filled however old the stick is) OK\n";
    }

    // ---- case 5: the same track, a different stick ------------------
    //
    // The whole point of the feature: the same recording on a rebuilt
    // stick, under a different path, mounted somewhere else. It is
    // matched on artist, title and length, so it lands on the row it
    // already had rather than adding a second one. A path-keyed store
    // would have made two.
    {
        const fs::path otherStick = root / "another-mount" / "REBUILT";
        writeFile(otherStick / "Music" / "2026" / "erste-remaster.mp3", "audio");
        MetadataStore metadata(db);
        Track moved = sampleTrack(otherStick, "Music/2026/erste-remaster.mp3", "Erste");
        moved.rating = 3;

        // Dated later than anything stored so far, so the rating it
        // brings actually lands. What this case is about is the row it
        // lands ON: one, not a second one filed under the new path.
        const auto summary = store(metadata, {moved}, sourceFor(otherStick, "REBUILT", CatalogNewest));
        assert(summary.tracksAdded == 0);
        assert(summary.tracksUpdated == 1);
        assert(metadata.trackCount() == 2);  // still two rows, not three
        const auto rows = metadata.browse("Erste", 10, 0);
        assert(rows[0].stickLabel == "REBUILT");
        assert(rows[0].relativePath == "Music/2026/erste-remaster.mp3");
        // The cues from the first stick are still there: this stick had
        // none to overwrite them with.
        assert(rows[0].cueCount == 2);
        std::cout << "case 5 (the same recording under another path is one row) OK\n";
    }

    // ---- case 5b: same artist and title, different length ------------
    //
    // A radio edit and an extended mix. Length is the guard that keeps
    // them apart; without it one would silently take the other's cues.
    {
        const fs::path mixDb = root / "mixes" / "metadata.db";
        MetadataStore metadata(mixDb);
        Track radioEdit = sampleTrack(stick, "Contents/A/edit.mp3", "One Track");
        radioEdit.durationSeconds = 210.0;
        radioEdit.cues = {hotCue(1, 1000.0)};
        Track extended = sampleTrack(stick, "Contents/A/extended.mp3", "One Track");
        extended.durationSeconds = 480.0;
        extended.cues = {hotCue(1, 9000.0)};

        store(metadata, {radioEdit, extended}, sourceFor(stick));
        assert(metadata.trackCount() == 2);

        // And a re-read of the shorter one, two seconds out, still finds
        // its own row rather than making a third.
        Track reread = radioEdit;
        reread.durationSeconds = 211.5;
        const auto again = store(metadata, {reread}, sourceFor(stick));
        assert(again.tracksAdded == 0);
        assert(metadata.trackCount() == 2);
        std::cout << "case 5b (same title, different length, two tracks) OK\n";
    }

    // ---- case 6: rows with no file on this stick --------------------
    {
        MetadataStore metadata(db);
        Track streaming = sampleTrack(stick, "Contents/Kalte Nacht/Erste.mp3", "Erste");
        streaming.streamingSource = "TIDAL";
        Track unresolved;
        unresolved.format = "engine";
        unresolved.title = "Nowhere";

        const auto summary = store(metadata, {streaming, unresolved}, sourceFor(stick));
        assert(summary.tracksSeen == 2);
        assert(summary.tracksWithoutIdentity == 2);
        assert(summary.tracksAdded == 0);
        assert(metadata.trackCount() == 2);
        std::cout << "case 6 (streaming links and rows with nothing to match on) OK\n";
    }

    // ---- case 7: unrated is not zero -------------------------------
    {
        const fs::path ratingDb = root / "ratings" / "metadata.db";
        MetadataStore metadata(ratingDb);
        Track zero = sampleTrack(stick, "Contents/A/zero.mp3", "Zero");
        zero.rating = 0;
        Track none = sampleTrack(stick, "Contents/A/none.mp3", "None");

        store(metadata, {zero, none}, sourceFor(stick));
        const auto rows = metadata.browse("", 10, 0);
        assert(rows.size() == 2);
        // Ordered by title within the one artist.
        assert(rows[0].title == "None" && !rows[0].rating.has_value());
        assert(rows[1].title == "Zero" && rows[1].rating.has_value() && *rows[1].rating == 0);
        std::cout << "case 7 (explicitly 0 stars survives as 0, unrated as absent) OK\n";
    }

    // ---- case 8: browse search and paging ---------------------------
    {
        MetadataStore metadata(db);
        assert(metadata.trackCount("erste") == 1);   // case-insensitive
        assert(metadata.trackCount("kalte") == 2);   // the artist matches too, not only the title
        assert(metadata.trackCount("zzz") == 0);
        assert(metadata.browse("", 1, 0).size() == 1);
        assert(metadata.browse("", 1, 1).size() == 1);
        assert(metadata.browse("", 1, 0)[0].id != metadata.browse("", 1, 1)[0].id);
        std::cout << "case 8 (search and paging) OK\n";
    }

    // ---- case 9: readAll carries cues, for matching -----------------
    {
        MetadataStore metadata(db);
        const auto tracks = metadata.readAll();
        assert(tracks.size() == 2);
        bool foundCues = false;
        for (const auto &track : tracks) {
            assert(track.format == "metadata-store");
            // No path at all, deliberately. matchTracks() treats an
            // exact path match as decisive and needs no other evidence,
            // which is right when both sides read one stick and wrong
            // for a store that outlives it. Empty is what guarantees
            // the store is always matched on title, artist and length.
            assert(track.filePath.empty());
            // And a date, so the merge rule has something to compare
            // against a stick's catalog mtime.
            assert(track.metadataModifiedAt > 0);
            if (track.title == "Erste") {
                assert(track.cues.size() == 2);
                foundCues = true;
            }
        }
        assert(foundCues);
        std::cout << "case 9 (readAll is a LibraryReader over the store) OK\n";
    }

    // ---- case 10: cancellation keeps what it wrote ------------------
    {
        const fs::path cancelDb = root / "cancel" / "metadata.db";
        MetadataStore metadata(cancelDb);
        CancellationToken token;
        token.cancel();
        Track track = sampleTrack(stick, "Contents/A/one.mp3", "One");
        const auto summary =
            metadata.store({track}, sourceFor(stick), NullProgressReporter::instance(), token);
        assert(summary.cancelled);
        assert(summary.tracksSeen == 0);
        assert(metadata.trackCount() == 0);
        std::cout << "case 10 (a cancelled run commits what it had) OK\n";
    }

    // ---- case 11: a database from another schema ---------------------
    //
    // Pre-1.0, the schema changes and there is no migration. What must
    // never happen is losing the file: this store may hold the only copy
    // of cues a reformatted stick no longer has.
    {
        const fs::path oldDb = root / "old-schema" / "metadata.db";
        fs::create_directories(oldDb.parent_path());
        {
            // A plausible earlier schema: a tracks table keyed the way
            // this store used to key it, and a version that is not ours.
            sqlite3 *raw = nullptr;
            assert(sqlite3_open(oldDb.string().c_str(), &raw) == SQLITE_OK);
            sqlite3_exec(raw, "CREATE TABLE tracks (id INTEGER PRIMARY KEY, path_key TEXT)", nullptr, nullptr,
                         nullptr);
            sqlite3_exec(raw, "INSERT INTO tracks (path_key) VALUES ('contents/a.mp3')", nullptr, nullptr, nullptr);
            sqlite3_exec(raw, "CREATE TABLE schema_version (version INTEGER NOT NULL)", nullptr, nullptr, nullptr);
            sqlite3_exec(raw, "INSERT INTO schema_version (version) VALUES (99)", nullptr, nullptr, nullptr);
            sqlite3_close(raw);
        }

        MetadataStore metadata(oldDb);
        // A working store, not a thrown error and not a broken one.
        Track track = sampleTrack(stick, "Contents/A/new.mp3", "New");
        track.cues = {memoryCue(0.0)};
        const auto summary = store(metadata, {track}, sourceFor(stick));
        assert(summary.tracksAdded == 1);
        assert(metadata.trackCount() == 1);

        // And the old file is still on disk, beside it.
        bool foundSuperseded = false;
        for (const auto &entry : fs::directory_iterator(oldDb.parent_path())) {
            if (entry.path().filename().string().find("metadata.db.superseded-99-") == 0) {
                foundSuperseded = true;
            }
        }
        assert(foundSuperseded);
        std::cout << "case 11 (a database from another schema is moved aside, never deleted) OK\n";
    }

    // ---- case 12: either key finds the row ---------------------------
    //
    // A track first catalogued with no artist is filed under its
    // filename. When the DJ fixes the tags, the next backup must land on
    // the row it already has -- the one holding the cues -- rather than
    // start a second one under the new spelling.
    {
        const fs::path keyDb = root / "keys" / "metadata.db";
        MetadataStore metadata(keyDb);

        Track untagged = sampleTrack(stick, "Contents/A/mystery.mp3", "");
        untagged.title.clear();
        untagged.artist.clear();
        untagged.cues = {hotCue(1, 5000.0), hotCue(2, 9000.0)};
        store(metadata, {untagged}, sourceFor(stick, "RV2", CatalogOld));
        assert(metadata.trackCount() == 1);
        assert(metadata.browse("", 10, 0)[0].cueCount == 2);

        Track tagged = sampleTrack(stick, "Contents/A/mystery.mp3", "Mystery");
        tagged.rating = 5;
        const auto summary = store(metadata, {tagged}, sourceFor(stick, "RV2", CatalogNew));
        assert(summary.tracksAdded == 0);
        assert(summary.tracksUpdated == 1);
        assert(metadata.trackCount() == 1);  // one track, not two
        auto rows = metadata.browse("", 10, 0);
        assert(rows[0].title == "Mystery");
        assert(rows[0].cueCount == 2);  // the cues it was stored with
        assert(*rows[0].rating == 5);

        // And back the other way: a catalog that has since lost the tags
        // still finds the row, and does not strip the strong key off it.
        Track lostTagsAgain = sampleTrack(stick, "Contents/A/mystery.mp3", "");
        lostTagsAgain.title.clear();
        lostTagsAgain.artist.clear();
        const auto again = store(metadata, {lostTagsAgain}, sourceFor(stick, "RV2", CatalogOld));
        assert(again.tracksAdded == 0);
        assert(metadata.trackCount() == 1);
        rows = metadata.browse("", 10, 0);
        // The title the row already had survives a reading that has none.
        assert(rows[0].title == "Mystery");
        std::cout << "case 12 (one row, found by title or by filename) OK\n";
    }

    // ---- case 13: version 1 is migrated, not moved aside -------------
    //
    // The exception to case 11, and the reason for it: a version-1 store
    // may hold the only copy of cues a reformatted stick no longer has,
    // and moving it aside would be a correct decision that still lost
    // them. One added column is worth migrating in place.
    {
        const fs::path oldDb = root / "schema-one" / "metadata.db";
        fs::create_directories(oldDb.parent_path());
        {
            sqlite3 *raw = nullptr;
            assert(sqlite3_open(oldDb.string().c_str(), &raw) == SQLITE_OK);
            // Version 1's tracks table, with the one key column it had.
            sqlite3_exec(raw,
                         "CREATE TABLE tracks (id INTEGER PRIMARY KEY, match_key TEXT NOT NULL, "
                         "relative_path TEXT NOT NULL, filename TEXT NOT NULL, "
                         "title TEXT NOT NULL DEFAULT '', artist TEXT NOT NULL DEFAULT '', "
                         "duration_seconds REAL NOT NULL DEFAULT 0, bpm REAL NOT NULL DEFAULT 0, "
                         "music_key TEXT NOT NULL DEFAULT '', rating INTEGER, "
                         "comment TEXT NOT NULL DEFAULT '', play_count INTEGER, "
                         "last_played_at TEXT NOT NULL DEFAULT '', artwork_sha TEXT NOT NULL DEFAULT '', "
                         "artwork_extension TEXT NOT NULL DEFAULT '', library_id TEXT NOT NULL DEFAULT '', "
                         "stick_label TEXT NOT NULL DEFAULT '', source_format TEXT NOT NULL DEFAULT '', "
                         "first_seen TEXT NOT NULL, updated_at TEXT NOT NULL)",
                         nullptr, nullptr, nullptr);
            sqlite3_exec(raw,
                         "CREATE TABLE cues (track_id INTEGER NOT NULL, kind TEXT NOT NULL, "
                         "hot_number INTEGER NOT NULL DEFAULT 0, position_ms REAL NOT NULL DEFAULT 0, "
                         "color TEXT NOT NULL DEFAULT '', comment TEXT NOT NULL DEFAULT '', "
                         "is_loop INTEGER NOT NULL DEFAULT 0, loop_end_ms REAL NOT NULL DEFAULT 0)",
                         nullptr, nullptr, nullptr);
            // One row keyed by title and artist, one keyed by filename.
            sqlite3_exec(raw,
                         "INSERT INTO tracks (match_key, relative_path, filename, title, artist, "
                         "duration_seconds, first_seen, updated_at) VALUES "
                         "('ta:kept|kaltenacht', 'Contents/A/kept.mp3', 'kept.mp3', 'Kept', 'Kalte Nacht', "
                         "361.5, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z')",
                         nullptr, nullptr, nullptr);
            sqlite3_exec(raw,
                         "INSERT INTO tracks (match_key, relative_path, filename, title, artist, "
                         "duration_seconds, first_seen, updated_at) VALUES "
                         "('fn:weak.mp3', 'Contents/A/weak.mp3', 'weak.mp3', '', '', "
                         "361.5, '2026-01-01T00:00:00Z', '2026-01-01T00:00:00Z')",
                         nullptr, nullptr, nullptr);
            sqlite3_exec(raw, "INSERT INTO cues (track_id, kind, position_ms) VALUES (1, 'memory', 4000)", nullptr,
                         nullptr, nullptr);
            sqlite3_exec(raw, "CREATE TABLE playlists (track_id INTEGER NOT NULL, name TEXT NOT NULL, "
                              "position INTEGER NOT NULL DEFAULT -1)",
                         nullptr, nullptr, nullptr);
            sqlite3_exec(raw, "CREATE TABLE schema_version (version INTEGER NOT NULL)", nullptr, nullptr, nullptr);
            sqlite3_exec(raw, "INSERT INTO schema_version (version) VALUES (1)", nullptr, nullptr, nullptr);
            sqlite3_close(raw);
        }

        MetadataStore metadata(oldDb);
        // Everything that was in there is still in there.
        assert(metadata.trackCount() == 2);
        const auto rows = metadata.browse("Kept", 10, 0);
        assert(rows.size() == 1);
        assert(rows[0].cueCount == 1);

        // Nothing was moved aside: the file itself was migrated.
        for (const auto &entry : fs::directory_iterator(oldDb.parent_path())) {
            assert(entry.path().filename().string().find("superseded") == std::string::npos);
        }

        // And the weak row's key was carried into the new column, so the
        // strong key can now replace it without the row losing the only
        // way it could still be found.
        Track nowTagged = sampleTrack(stick, "Contents/A/weak.mp3", "Weak");
        const auto summary = store(metadata, {nowTagged}, sourceFor(stick, "RV2", CatalogNew));
        assert(summary.tracksAdded == 0);
        assert(metadata.trackCount() == 2);
        std::cout << "case 13 (a version-1 store is migrated in place, keeping its cues) OK\n";
    }

    // ---- case 14: deleting stored tracks ----------------------------
    {
        const fs::path deleteDb = root / "deletes" / "metadata.db";
        MetadataStore metadata(deleteDb);
        Track first = sampleTrack(stick, "Contents/A/one.mp3", "One");
        first.cues = {memoryCue(0.0), hotCue(1, 1000.0)};
        first.playlists = {PlaylistMembership{"Techno", 0}};
        Track second = sampleTrack(stick, "Contents/A/two.mp3", "Two");
        second.cues = {memoryCue(0.0)};
        store(metadata, {first, second}, sourceFor(stick));
        assert(metadata.trackCount() == 2);

        const auto rows = metadata.browse("One", 10, 0);
        assert(rows.size() == 1);
        const std::int64_t goneId = rows[0].id;
        assert(metadata.removeTracks({goneId}) == 1);
        assert(metadata.trackCount() == 1);
        // The cues and playlist rows went with it, rather than being
        // left owned by nothing.
        assert(metadata.cuesFor(goneId).empty());
        assert(metadata.playlistsFor(goneId).empty());
        // The track that was not asked for is untouched.
        assert(metadata.browse("Two", 10, 0).size() == 1);
        assert(metadata.browse("Two", 10, 0)[0].cueCount == 1);
        // A second delete of the same id removes nothing and says so.
        assert(metadata.removeTracks({goneId}) == 0);
        assert(metadata.removeTracks({}) == 0);
        std::cout << "case 14 (deleting a stored track takes its cues with it) OK\n";
    }

    // ---- case 15: a re-run does not re-date what it did not change ---
    //
    // From a review finding. authored_at used to be updated_at, which
    // every pass over a row bumped to the clock whether or not anything
    // was written. A routine backup of a stick you had not touched then
    // dated the store later than work done on another stick in between,
    // and the merge rule preferred the store to that work -- silently,
    // and in the direction that loses the newer cues.
    {
        const fs::path driftDb = root / "drift" / "metadata.db";
        MetadataStore metadata(driftDb);

        // Day 1: stick A, comment "from A".
        Track onA = sampleTrack(stick, "Contents/A/shared.mp3", "Shared");
        onA.comment = "from A";
        store(metadata, {onA}, sourceFor(stick, "A", CatalogOld));

        // Day 3: a routine re-run over A, which has not been touched.
        // Nothing is written, and nothing may be re-dated either.
        const auto rerun = store(metadata, {onA}, sourceFor(stick, "A", CatalogMid));
        assert(rerun.tracksUnchanged == 1);
        assert(rerun.tracksUpdated == 0);

        // Day 6: stick B, whose catalogs were written between the two
        // runs above, carrying a comment the DJ wrote there. It is newer
        // than the stored value's own date and must win -- which it only
        // does if the re-run left that date alone.
        Track onB = sampleTrack(stick, "Contents/A/shared.mp3", "Shared");
        onB.comment = "from B";
        const auto fromB = store(metadata, {onB}, sourceFor(stick, "B", CatalogMid));
        assert(fromB.tracksUpdated == 1);
        assert(metadata.browse("Shared", 10, 0)[0].comment == "from B");
        std::cout << "case 15 (a run that changes nothing re-dates nothing) OK\n";
    }

    // ---- case 16: a stored value keeps the date of the stick it came from
    //
    // The other half of the same finding. A value read off a stick is as
    // old as that stick's catalogs, not as young as the moment Seabass
    // happened to read it -- otherwise plugging in an old stick would
    // make its months-old cues beat a newer stick's simply by being read
    // second.
    {
        const fs::path provenanceDb = root / "provenance" / "metadata.db";
        MetadataStore metadata(provenanceDb);

        // An old stick, read today.
        Track old = sampleTrack(stick, "Contents/A/prov.mp3", "Prov");
        old.comment = "ancient";
        store(metadata, {old}, sourceFor(stick, "OLD", CatalogOld));

        // A stick whose catalogs are newer than that one's, but still
        // long in the past. It wins on its own date, not on when it was
        // read.
        Track newer = sampleTrack(stick, "Contents/A/prov.mp3", "Prov");
        newer.comment = "recent";
        const auto summary = store(metadata, {newer}, sourceFor(stick, "NEW", CatalogMid));
        assert(summary.tracksUpdated == 1);
        assert(metadata.browse("Prov", 10, 0)[0].comment == "recent");

        // And the reverse order gives the same answer, which is the
        // whole point: the outcome is a property of the two sticks, not
        // of which was plugged in last.
        const auto backwards = store(metadata, {old}, sourceFor(stick, "OLD", CatalogOld));
        assert(backwards.tracksSkipped == 1);
        assert(metadata.browse("Prov", 10, 0)[0].comment == "recent");
        std::cout << "case 16 (a stored value carries its source stick's date) OK\n";
    }

    // ---- case 17: a filename never adopts another track's row --------
    //
    // From a review finding, and the scenario is specific enough that a
    // looser one passes against the bug. Three things have to line up:
    // the incoming track's artist and title must name a stored row, that
    // row must be rejected on length, and a DIFFERENT row must share the
    // incoming filename and agree on length.
    //
    // That is a radio edit and an extended mix of one song, plus any
    // unrelated recording exported under the same basename -- "01.mp3",
    // "track01.mp3", "audio.mp3" -- which is ordinary on a stick built
    // by dragging folders around.
    //
    // The old lookup ORed both keys into one query. The radio edit was
    // rejected on length, the loop fell through to the unrelated row,
    // adopted it, and the UPDATE stamped it with the extended mix's
    // title and artist. Another track's cues, relabelled, with no error
    // anywhere. A strong-key hit has to be decisive even when it ends in
    // no match.
    {
        const fs::path collisionDb = root / "collision" / "metadata.db";
        MetadataStore metadata(collisionDb);

        // The radio edit: same artist and title as what arrives later,
        // three and a half minutes long, filed under mix.mp3.
        Track radioEdit = sampleTrack(stick, "Contents/A/mix.mp3", "One Track");
        radioEdit.durationSeconds = 210.0;
        radioEdit.cues = {hotCue(1, 1000.0)};

        // An unrelated recording that happens to share that basename,
        // and happens to be as long as the extended mix.
        Track unrelated = sampleTrack(stick, "Contents/B/mix.mp3", "Quite Other");
        unrelated.artist = "Someone Else";
        unrelated.durationSeconds = 480.0;
        unrelated.cues = {hotCue(1, 5000.0), hotCue(2, 9000.0)};
        unrelated.comment = "not to be touched";

        store(metadata, {radioEdit, unrelated}, sourceFor(stick, "RV2", CatalogOld));
        assert(metadata.trackCount() == 2);

        // The extended mix arrives: strong key names the radio edit,
        // which is rejected on length; filename names the unrelated row,
        // whose length agrees.
        Track extended = sampleTrack(stick, "Contents/A/mix.mp3", "One Track");
        extended.durationSeconds = 480.0;
        extended.cues = {hotCue(1, 400000.0)};

        const auto summary = store(metadata, {extended}, sourceFor(stick, "RV2", CatalogNew));
        assert(summary.tracksAdded == 1);    // a row of its own
        assert(summary.tracksUpdated == 0);  // nobody else's
        assert(metadata.trackCount() == 3);

        // The unrelated recording still says what it always said.
        const auto others = metadata.browse("Quite Other", 10, 0);
        assert(others.size() == 1);
        assert(others[0].artist == "Someone Else");
        assert(others[0].comment == "not to be touched");
        assert(others[0].cueCount == 2);
        std::cout << "case 17 (a shared filename does not hand over another track's row) OK\n";
    }

    fs::remove_all(root);
    std::cout << "all metadata_store_test cases passed\n";
    return 0;
}
