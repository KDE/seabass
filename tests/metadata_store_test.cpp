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

// Two dates for the stick side of the merge rule, either side of the
// one the store itself stamps on a row (which is "now"). A catalog
// written in 2033 is more recent than any row this test can write;
// one from 2023 is older than all of them.
constexpr std::int64_t CatalogWrittenBeforeAnyRow = 1'700'000'000;   // 2023
constexpr std::int64_t CatalogWrittenAfterEveryRow = 2'000'000'000;  // 2033

MetadataSource sourceFor(const fs::path &stickRoot, const std::string &label = "RV2",
                          std::int64_t catalogModifiedAt = CatalogWrittenAfterEveryRow)
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
    // Every dated case below assumes the rows this test writes -- which
    // the store stamps with the wall clock -- fall between these two.
    // Said out loud rather than left to be discovered in 2033, when the
    // upper bound passes and half these assertions start failing for a
    // reason that has nothing to do with the code under test.
    {
        const auto now = static_cast<std::int64_t>(std::time(nullptr));
        assert(now > CatalogWrittenBeforeAnyRow && now < CatalogWrittenAfterEveryRow);
    }

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

        const auto kept = store(metadata, {changed}, sourceFor(stick, "RV2", CatalogWrittenBeforeAnyRow));
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
        const auto taken = store(metadata, {changed}, sourceFor(stick, "RV2", CatalogWrittenAfterEveryRow));
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
        store(metadata, {full}, sourceFor(stick, "RV2", CatalogWrittenBeforeAnyRow));

        Track reExported = sampleTrack(stick, "Contents/Kalte Nacht/Erste.mp3", "Erste");
        reExported.cues = {memoryCue(0.0)};
        const auto summary = store(metadata, {reExported}, sourceFor(stick, "RV2", CatalogWrittenAfterEveryRow));
        assert(summary.tracksSkipped == 1);
        assert(summary.tracksUpdated == 0);
        const auto rows = metadata.browse("Erste", 10, 0);
        assert(rows[0].cueCount == 3);

        // And the other direction: a stick that has gained cues gives
        // them to a store that has fewer, however old the catalog is.
        Track recued = sampleTrack(stick, "Contents/Kalte Nacht/Erste.mp3", "Erste");
        recued.cues = {memoryCue(0.0), hotCue(1, 32000.0), hotCue(2, 64000.0), hotCue(3, 96000.0)};
        const auto grew = store(metadata, {recued}, sourceFor(stick, "RV2", CatalogWrittenBeforeAnyRow));
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
        const auto summary = store(metadata, {second}, sourceFor(stick, "RV2", CatalogWrittenBeforeAnyRow));
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

        const auto summary = store(metadata, {moved}, sourceFor(otherStick, "REBUILT"));
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
        store(metadata, {untagged}, sourceFor(stick, "RV2", CatalogWrittenBeforeAnyRow));
        assert(metadata.trackCount() == 1);
        assert(metadata.browse("", 10, 0)[0].cueCount == 2);

        Track tagged = sampleTrack(stick, "Contents/A/mystery.mp3", "Mystery");
        tagged.rating = 5;
        const auto summary = store(metadata, {tagged}, sourceFor(stick, "RV2", CatalogWrittenAfterEveryRow));
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
        const auto again = store(metadata, {lostTagsAgain}, sourceFor(stick, "RV2", CatalogWrittenBeforeAnyRow));
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
        const auto summary = store(metadata, {nowTagged}, sourceFor(stick, "RV2", CatalogWrittenAfterEveryRow));
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

    fs::remove_all(root);
    std::cout << "all metadata_store_test cases passed\n";
    return 0;
}
