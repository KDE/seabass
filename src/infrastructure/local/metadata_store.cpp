#include "infrastructure/local/metadata_store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

#include "domain/metadata_merge.hpp"
#include "domain/track_matching.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/local/sqlite_statement.hpp"
#include "infrastructure/paths/seabass_paths.hpp"

namespace seabass::infrastructure::local
{

namespace fs = std::filesystem;
using domain::CuePoint;
using domain::PlaylistMembership;
using domain::Track;

namespace
{

constexpr const char *Context = "metadata store";
// 2 added fallback_key, so a row first stored under a filename (because
// the catalog had no artist and title yet) is still found once those
// arrive, instead of being filed a second time as a new track.
constexpr int SchemaVersion = 2;

// The shared RAII statement and exec() with this store's error-message
// context bound in, so call sites stay two arguments.
struct Stmt : Statement
{
    Stmt(sqlite3 *db, const char *sql) : Statement(db, sql, Context) {}
};

void exec(sqlite3 *db, const char *sql)
{
    local::exec(db, sql, Context);
}

// How a track is spelled for display once it is off the stick.
//
// Stick-relative, because an absolute path is a claim about a mount
// point that will not be there next time. Display only: it is not what
// tracks are matched on. Purely lexical -- lexically_relative rather
// than fs::relative -- so it never touches the filesystem and gives the
// same answer for a stick that is no longer plugged in.
std::string stickRelativePath(const std::string &filePath, const fs::path &stickRoot)
{
    if (filePath.empty()) {
        return {};
    }
    if (!stickRoot.empty()) {
        fs::path relative = fs::path(filePath).lexically_normal().lexically_relative(stickRoot.lexically_normal());
        const std::string text = relative.generic_string();
        // lexically_relative walks up with ".." when the path is not
        // under the root at all; such an answer says nothing about the
        // stick and must not become a key.
        if (!text.empty() && text != "." && text.rfind("..", 0) != 0) {
            return text;
        }
    }
    // A path we cannot place on the stick still has a filename, and a
    // filename is a weaker key rather than no key. Losing the track
    // entirely would be worse.
    return fs::path(filePath).filename().generic_string();
}

// What two rows have to agree on to be the same track: artist, title
// and length, the same rule domain::matchTracks() uses everywhere else
// in Seabass, with the filename as the fallback matchTracks also uses.
//
// Not the file path. A path is the strongest signal when both sides are
// looking at one stick, and the weakest thing to key a store on: the
// whole point of this store is that it outlives the stick, and a
// re-export renames folders, a rebuilt library moves Contents/ around,
// and the same track bought again lands somewhere else entirely.
// Artist and title travel with the recording.
//
// Length is not part of the key, because two readings of one file differ
// by rounding; it is a guard applied to the candidates the key finds,
// with the same tolerance and the same "only when both readings are
// real" rule matchTracks uses. A zero duration means unreadable, not a
// zero-length track, and gating on it would split one track into two
// rows the moment one catalog failed to report a length.
constexpr double DurationToleranceSeconds = 2.0;

// "2026-09-11T21:55:00Z" as seconds since the epoch, 0 for anything
// that is not that shape.
//
// Hand-parsed rather than via std::get_time, which needs a locale-bound
// stream to read a format this store writes itself, and timegm(), which
// is not portable. The format is fixed at the one call site that
// produces it (isoTimestampUtc), so there is nothing here to be liberal
// about.
std::int64_t epochFromIsoTimestamp(const std::string &text)
{
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    if (std::sscanf(text.c_str(), "%d-%2u-%2uT%2u:%2u:%2uZ", &year, &month, &day, &hour, &minute, &second) != 6) {
        return 0;
    }
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31) {
        return 0;
    }
    // Days since 1970-01-01 by Howard Hinnant's days_from_civil, the
    // standard branch-free form of the calculation std::chrono's own
    // year_month_day uses. Shifting the year to start in March makes
    // the leap day the last day of the year, which is what removes
    // every special case from the arithmetic.
    const int y = static_cast<int>(year) - (month <= 2 ? 1 : 0);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (month + (month > 2 ? -3 : 9)) + 2u) / 5u + day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
    return days * 86400 + static_cast<std::int64_t>(hour) * 3600 + static_cast<std::int64_t>(minute) * 60 + second;
}

// The strong key: normalized artist and title, or empty when the track
// does not have both.
std::string titleArtistMatchKey(const Track &track)
{
    if (auto key = domain::titleArtistKey(track)) {
        return "ta:" + *key;
    }
    return {};
}

// The weak one, kept alongside rather than instead of the strong one.
//
// Storing only whichever key happened to be available was the bug: a
// track catalogued without an artist got filed under its filename, and
// when the DJ later fixed the tags the next backup looked it up under
// "ta:" only, found nothing, and stored a second row -- with the first
// row still holding the cues. Two keys per row, either of which can
// find it, is what makes that one track instead of two.
std::string filenameMatchKey(const Track &track)
{
    const std::string filename = domain::normalizeFilename(track.filename);
    return filename.empty() ? std::string() : "fn:" + filename;
}

bool durationsAgree(double a, double b)
{
    if (a <= 0.0 || b <= 0.0) {
        return true;
    }
    return std::abs(a - b) <= DurationToleranceSeconds;
}

std::string lowercased(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string kindToText(CuePoint::Kind kind)
{
    return kind == CuePoint::Kind::Hot ? "hot" : "memory";
}

CuePoint::Kind kindFromText(const std::string &text)
{
    return text == "hot" ? CuePoint::Kind::Hot : CuePoint::Kind::Memory;
}

std::string readWholeFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

// ---- construction ---------------------------------------------------

MetadataStore::MetadataStore(fs::path databasePath) : m_databasePath(std::move(databasePath))
{
    openAndMigrate();
}

MetadataStore::~MetadataStore()
{
    if (m_db) {
        sqlite3_close(m_db);
    }
}

fs::path MetadataStore::defaultDatabasePath()
{
    return paths::localMetadataDir() / "metadata.db";
}

fs::path MetadataStore::artworkDir() const
{
    return m_databasePath.parent_path() / "artwork";
}

// The version an existing database was written with, or 0 when it has
// none (no file, or one written before schema_version existed).
namespace
{

int schemaVersionOf(const fs::path &databasePath)
{
    std::error_code ec;
    if (!fs::exists(databasePath, ec)) {
        return 0;
    }
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(databasePath.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;  // there is a file, and it is not a database we can read
    }
    int version = 0;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT version FROM schema_version", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            version = sqlite3_column_int(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return version;
}

}  // namespace

void MetadataStore::openAndMigrate()
{
    std::error_code ec;
    fs::create_directories(m_databasePath.parent_path(), ec);

    // A database from a different schema is moved aside rather than
    // migrated or deleted.
    //
    // Seabass is pre-1.0 and does not pay back-compatibility tax, so
    // there is no migration to write. But this store is the one place
    // that may hold cues no stick has any more, so deleting it is not
    // available either: "we changed the schema" is not a reason to lose
    // a DJ's work. Renaming costs nothing, leaves the file where a
    // person can find it, and CREATE TABLE IF NOT EXISTS then builds a
    // clean one -- which a single Back Up Now refills from the stick.
    //
    // 1 -> 2 is the exception, and the reason the exception exists is
    // the paragraph above. Moving a version-1 store aside would be
    // correct and would still lose exactly the cues this feature is for:
    // a stick that has been reformatted since the backup cannot refill
    // it. The change is one added column, so it is migrated in place
    // once the database is open, below.
    const int existing = schemaVersionOf(m_databasePath);
    const bool migrateInPlace = existing == 1 && SchemaVersion == 2;
    if (!migrateInPlace && existing != 0 && existing != SchemaVersion) {
        // isoTimestampUtc()'s colons (the "T14:23:45Z" part) are fine as
        // a stored value -- every other call site uses it that way --
        // but not as part of a filename: NTFS reads a colon there as the
        // start of an Alternate Data Stream name, so fs::rename() failed
        // to find any such "file", set ec, and this correctly-but-
        // needlessly threw "could not move it aside" on every Windows
        // run that ever hit this path.
        std::string timestamp = isoTimestampUtc();
        std::replace(timestamp.begin(), timestamp.end(), ':', '-');
        fs::path superseded = m_databasePath;
        superseded += ".superseded-" + std::to_string(existing) + "-" + timestamp;
        fs::rename(m_databasePath, superseded, ec);
        if (ec) {
            throw std::runtime_error(std::string(Context) + ": found a database written by another version of "
                                      "Seabass and could not move it aside: " + ec.message());
        }
    }

    if (sqlite3_open(m_databasePath.string().c_str(), &m_db) != SQLITE_OK) {
        const std::string message = m_db ? sqlite3_errmsg(m_db) : "could not open database";
        sqlite3_close(m_db);
        m_db = nullptr;
        throw std::runtime_error(std::string(Context) + ": " + message);
    }

    // Two connections exist in the running app: the one the browse view
    // reads through and the one a backup writes through. A reader that
    // arrives mid-transaction should wait rather than fail the page.
    sqlite3_busy_timeout(m_db, 5000);
    exec(m_db, "PRAGMA foreign_keys = ON;");

    // In place, before the CREATE TABLEs below: they are all IF NOT
    // EXISTS, so they would leave a version-1 tracks table exactly as it
    // is and the added column would never appear.
    if (migrateInPlace) {
        exec(m_db, "ALTER TABLE tracks ADD COLUMN fallback_key TEXT NOT NULL DEFAULT '';");
        // Every version-1 row carries one key, in a column that says
        // which kind it is. A row keyed by filename already has its
        // fallback key; it just has it in the other column, and copying
        // it across is what lets the strong key replace it later
        // without the row losing the only way it can still be found.
        exec(m_db, "UPDATE tracks SET fallback_key = match_key WHERE match_key LIKE 'fn:%';");
        exec(m_db, "UPDATE schema_version SET version = 2;");
    }

    exec(m_db, R"sql(
        CREATE TABLE IF NOT EXISTS tracks (
            id INTEGER PRIMARY KEY,
            match_key TEXT NOT NULL,
            fallback_key TEXT NOT NULL DEFAULT '',
            relative_path TEXT NOT NULL,
            filename TEXT NOT NULL,
            title TEXT NOT NULL DEFAULT '',
            artist TEXT NOT NULL DEFAULT '',
            duration_seconds REAL NOT NULL DEFAULT 0,
            bpm REAL NOT NULL DEFAULT 0,
            music_key TEXT NOT NULL DEFAULT '',
            rating INTEGER,
            comment TEXT NOT NULL DEFAULT '',
            play_count INTEGER,
            last_played_at TEXT NOT NULL DEFAULT '',
            artwork_sha TEXT NOT NULL DEFAULT '',
            artwork_extension TEXT NOT NULL DEFAULT '',
            library_id TEXT NOT NULL DEFAULT '',
            stick_label TEXT NOT NULL DEFAULT '',
            source_format TEXT NOT NULL DEFAULT '',
            first_seen TEXT NOT NULL,
            updated_at TEXT NOT NULL
        );
    )sql");
    exec(m_db, R"sql(
        CREATE TABLE IF NOT EXISTS cues (
            track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
            kind TEXT NOT NULL,
            hot_number INTEGER NOT NULL DEFAULT 0,
            position_ms REAL NOT NULL DEFAULT 0,
            color TEXT NOT NULL DEFAULT '',
            comment TEXT NOT NULL DEFAULT '',
            is_loop INTEGER NOT NULL DEFAULT 0,
            loop_end_ms REAL NOT NULL DEFAULT 0
        );
    )sql");
    exec(m_db, R"sql(
        CREATE TABLE IF NOT EXISTS playlists (
            track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
            name TEXT NOT NULL,
            position INTEGER NOT NULL DEFAULT -1
        );
    )sql");
    exec(m_db, "CREATE INDEX IF NOT EXISTS tracks_by_match_key ON tracks(match_key);");
    exec(m_db, "CREATE INDEX IF NOT EXISTS tracks_by_fallback_key ON tracks(fallback_key);");
    exec(m_db, "CREATE INDEX IF NOT EXISTS cues_by_track ON cues(track_id);");
    exec(m_db, "CREATE INDEX IF NOT EXISTS playlists_by_track ON playlists(track_id);");
    exec(m_db, "CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL);");

    Stmt version(m_db, "SELECT version FROM schema_version");
    if (!version.step()) {
        Stmt insert(m_db, "INSERT INTO schema_version (version) VALUES (?)");
        insert.bind(1, SchemaVersion);
        insert.run();
    }
}

// ---- artwork --------------------------------------------------------

namespace
{

struct ArtworkIntake
{
    std::string sha;
    std::string extension;
    bool copied = false;
    std::uint64_t bytes = 0;
};

// Copies a cover image next to the database under its own SHA-256, and
// answers with the hash the row should keep.
//
// Content-addressed rather than one copy per track: a library where 1500
// tracks share 300 album covers stores 300 files, and a second backup run
// over the same stick writes none of them again. The extension is kept
// alongside the hash so a viewer can be handed a path with a type on it;
// the hash alone would leave every image extensionless.
ArtworkIntake intakeArtwork(const std::string &sourcePath, const fs::path &artworkDir)
{
    ArtworkIntake intake;
    if (sourcePath.empty()) {
        return intake;
    }
    const std::string bytes = readWholeFile(sourcePath);
    if (bytes.empty()) {
        return intake;
    }

    intake.sha = hashing::toHex(hashing::Sha256::of(bytes));
    intake.extension = lowercased(fs::path(sourcePath).extension().generic_string());
    if (intake.extension.empty()) {
        intake.extension = ".jpg";
    }

    std::error_code ec;
    fs::create_directories(artworkDir, ec);
    const fs::path destination = artworkDir / (intake.sha + intake.extension);
    if (fs::exists(destination, ec)) {
        return intake;
    }

    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) {
        // A cover we cannot copy is not a reason to lose the track's
        // cues. Drop the image, keep the row.
        intake.sha.clear();
        intake.extension.clear();
        return intake;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
        fs::remove(destination, ec);
        intake.sha.clear();
        intake.extension.clear();
        return intake;
    }

    intake.copied = true;
    intake.bytes = bytes.size();
    return intake;
}

}  // namespace

std::uint64_t MetadataStore::artworkBytesOnDisk() const
{
    std::error_code ec;
    std::uint64_t total = 0;
    for (const auto &entry : fs::directory_iterator(artworkDir(), ec)) {
        if (entry.is_regular_file(ec)) {
            total += entry.file_size(ec);
        }
    }
    return total;
}

// ---- writing --------------------------------------------------------

MetadataBackupSummary MetadataStore::store(const std::vector<Track> &tracks, const MetadataSource &source,
                                            application::ProgressReporter &progress,
                                            const application::CancellationToken &cancel)
{
    MetadataBackupSummary summary;
    const std::string now = isoTimestampUtc();
    const fs::path artwork = artworkDir();

    progress.start("Storing metadata", tracks.size());
    exec(m_db, "BEGIN IMMEDIATE");
    // Any throw between here and COMMIT must not leave the database in a
    // transaction; a ROLLBACK on an already-finished transaction is
    // harmless, one that never runs is not.
    struct RollbackGuard
    {
        sqlite3 *db;
        bool committed = false;
        ~RollbackGuard()
        {
            if (!committed) {
                sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            }
        }
    } guard{m_db};

    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (cancel.cancelled()) {
            summary.cancelled = true;
            break;
        }
        progress.tick(i);

        const Track &track = tracks[i];
        summary.tracksSeen++;

        // A streaming link names no file on any stick -- its path
        // points into a cache on whatever computer manages playback --
        // so there is nothing a restore could ever put back on it.
        if (!track.streamingSource.empty()) {
            summary.tracksWithoutIdentity++;
            continue;
        }
        const std::string matchKey = titleArtistMatchKey(track);
        const std::string fallbackKey = filenameMatchKey(track);
        if (matchKey.empty() && fallbackKey.empty()) {
            // No artist and title, and no filename either. There is
            // nothing to recognise this row by later, so storing it
            // would only ever produce a row nothing can match.
            summary.tracksWithoutIdentity++;
            continue;
        }
        const std::string relativePath = stickRelativePath(track.filePath, source.stickRoot);

        std::int64_t id = 0;
        bool exists = false;
        std::optional<int> storedRating;
        std::optional<int> storedPlayCount;
        std::string storedComment;
        std::string storedArtworkSha;
        std::int64_t storedModifiedAt = 0;
        {
            // Every row either key could mean, then the first whose
            // length agrees. Two rows under one key is the real case the
            // length guard handles: a radio edit and an extended mix.
            //
            // Either key, not just the strong one, so a row first stored
            // under a filename is found again once the DJ fixes the tags
            // -- and so a row stored with full tags is still found by a
            // catalog that has since lost them. Both spellings of the
            // same track reach the same row instead of making a second.
            // The strong key is tried first and on its own: a filename
            // is a weak enough key that letting it pull in candidates
            // while a better one is available would be a way to merge
            // two genuinely different tracks that share a name.
            const char *sql =
                "SELECT id, rating, comment, play_count, artwork_sha, duration_seconds, updated_at "
                "FROM tracks WHERE (? <> '' AND match_key = ?) OR (? <> '' AND fallback_key = ?) "
                "ORDER BY (match_key = ?) DESC, id";
            Stmt find(m_db, sql);
            find.bind(1, matchKey);
            find.bind(2, matchKey);
            find.bind(3, fallbackKey);
            find.bind(4, fallbackKey);
            find.bind(5, matchKey);
            while (find.step()) {
                if (!durationsAgree(find.columnDouble(5), track.durationSeconds)) {
                    continue;
                }
                exists = true;
                id = find.columnInt64(0);
                storedRating = find.columnOptionalInt(1);
                storedComment = find.columnText(2);
                storedPlayCount = find.columnOptionalInt(3);
                storedArtworkSha = find.columnText(4);
                storedModifiedAt = epochFromIsoTimestamp(find.columnText(6));
                break;
            }
        }

        std::vector<CuePoint> storedCues;
        if (exists) {
            storedCues = cuesFor(id);
        }

        // The shared merge rule, per authored field group: a blank is
        // filled, more cues wins, otherwise the later edit wins. The
        // stick is the incoming side here and the store the existing
        // one; the restore path runs the same rule with the two the
        // other way round, which is the whole point of it living in the
        // domain rather than here.
        const bool writeCues =
            domain::takeIncomingCues(track.cues, storedCues, source.catalogModifiedAt, storedModifiedAt);
        const bool writeRating =
            domain::takeIncomingRating(track.rating, storedRating, source.catalogModifiedAt, storedModifiedAt);
        const bool writeComment =
            domain::takeIncomingComment(track.comment, storedComment, source.catalogModifiedAt, storedModifiedAt);
        const bool writePlayCount =
            domain::takeIncomingPlayCount(track.playCount, storedPlayCount, source.catalogModifiedAt,
                                           storedModifiedAt);

        // A track the rule decided against on at least one group: the
        // stick offered something, the store already had something else,
        // and what was stored won.
        const bool cuesConflict = !storedCues.empty() && !track.cues.empty() &&
                                   !domain::cueSetsEqual(storedCues, track.cues);
        const bool ratingConflict = storedRating.has_value() && track.rating.has_value() &&
                                     *storedRating != *track.rating;
        const bool commentConflict = !storedComment.empty() && !track.comment.empty() &&
                                      storedComment != track.comment;
        const bool playCountConflict = storedPlayCount.has_value() && track.playCount.has_value() &&
                                        *storedPlayCount != *track.playCount;

        // Artwork is only fetched when the row has none: a cover is not
        // authored data, so a stored one is as good as an incoming one
        // and re-hashing every image on every run would make a second
        // backup cost as much as the first.
        ArtworkIntake artworkIntake;
        if (storedArtworkSha.empty() && !track.artworkPath.empty()) {
            artworkIntake = intakeArtwork(track.artworkPath, artwork);
            if (artworkIntake.copied) {
                summary.artworkFilesAdded++;
                summary.artworkBytesAdded += artworkIntake.bytes;
            }
        }

        std::string lastPlayed;
        if (track.lastPlayedAt) {
            const std::time_t asTime = std::chrono::system_clock::to_time_t(*track.lastPlayedAt);
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm, &asTime);
#else
            gmtime_r(&asTime, &tm);
#endif
            char buffer[32];
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
            lastPlayed = buffer;
        }

        if (!exists) {
            Stmt insert(m_db, R"sql(
                INSERT INTO tracks (match_key, fallback_key, relative_path, filename, title, artist,
                                    duration_seconds, bpm, music_key, rating, comment,
                                    play_count, last_played_at, artwork_sha, artwork_extension,
                                    library_id, stick_label, source_format, first_seen, updated_at)
                VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
            )sql");
            insert.bind(1, matchKey);
            insert.bind(2, fallbackKey);
            insert.bind(3, relativePath);
            insert.bind(4, track.filename);
            insert.bind(5, track.title);
            insert.bind(6, track.artist);
            insert.bind(7, track.durationSeconds);
            insert.bind(8, track.bpm);
            insert.bind(9, track.key);
            insert.bind(10, track.rating);
            insert.bind(11, track.comment);
            insert.bind(12, track.playCount);
            insert.bind(13, lastPlayed);
            insert.bind(14, artworkIntake.sha);
            insert.bind(15, artworkIntake.extension);
            insert.bind(16, source.libraryId);
            insert.bind(17, source.stickLabel);
            insert.bind(18, track.format);
            insert.bind(19, now);
            insert.bind(20, now);
            insert.run();
            id = sqlite3_last_insert_rowid(m_db);
            summary.tracksAdded++;
        }

        // Identity fields (title, artist, duration, bpm, key, where it
        // was last seen) always refresh: they are what the software
        // wrote, not what the DJ authored, and the freshest reading of
        // them is the best one.
        if (exists) {
            Stmt update(m_db, R"sql(
                UPDATE tracks SET match_key = CASE WHEN ? <> '' THEN ? ELSE match_key END,
                                  fallback_key = CASE WHEN ? <> '' THEN ? ELSE fallback_key END,
                                  relative_path = CASE WHEN ? <> '' THEN ? ELSE relative_path END,
                                  filename = CASE WHEN ? <> '' THEN ? ELSE filename END,
                                  title = CASE WHEN ? <> '' THEN ? ELSE title END,
                                  artist = CASE WHEN ? <> '' THEN ? ELSE artist END,
                                  duration_seconds = CASE WHEN ? > 0 THEN ? ELSE duration_seconds END,
                                  bpm = CASE WHEN ? > 0 THEN ? ELSE bpm END,
                                  music_key = CASE WHEN ? <> '' THEN ? ELSE music_key END,
                                  library_id = ?, stick_label = ?, source_format = ?, updated_at = ?
                WHERE id = ?
            )sql");
            // Both keys refresh with the rest: a title corrected on the
            // stick has to be findable under the corrected spelling, or
            // the next backup files it as a second track.
            //
            // Every identity field is guarded the same way, and it is
            // the same guarantee the merge rule's first step makes about
            // authored fields: a reading that is empty is not a reading.
            // A catalog that has lost a track's tags reports no title
            // and no artist, and writing that straight through would
            // blank the title on a row that has one and clear the strong
            // key off it -- leaving the entry findable by filename
            // alone, or by nothing, and showing as a bare filename in
            // the list. A zero duration or bpm means unreadable rather
            // than a zero-length track, so those are guarded on > 0 for
            // exactly the same reason.
            //
            // Where a reading does have a value it still wins outright:
            // these are what the software wrote rather than what the DJ
            // authored, so the freshest real reading of them is the best
            // one, and no date needs consulting.
            update.bind(1, matchKey);
            update.bind(2, matchKey);
            update.bind(3, fallbackKey);
            update.bind(4, fallbackKey);
            update.bind(5, relativePath);
            update.bind(6, relativePath);
            update.bind(7, track.filename);
            update.bind(8, track.filename);
            update.bind(9, track.title);
            update.bind(10, track.title);
            update.bind(11, track.artist);
            update.bind(12, track.artist);
            update.bind(13, track.durationSeconds);
            update.bind(14, track.durationSeconds);
            update.bind(15, track.bpm);
            update.bind(16, track.bpm);
            update.bind(17, track.key);
            update.bind(18, track.key);
            update.bind(19, source.libraryId);
            update.bind(20, source.stickLabel);
            update.bind(21, track.format);
            update.bind(22, now);
            update.bindInt64(23, id);
            update.run();

            if (writeRating) {
                Stmt set(m_db, "UPDATE tracks SET rating = ? WHERE id = ?");
                set.bind(1, track.rating);
                set.bindInt64(2, id);
                set.run();
            }
            if (writeComment) {
                Stmt set(m_db, "UPDATE tracks SET comment = ? WHERE id = ?");
                set.bind(1, track.comment);
                set.bindInt64(2, id);
                set.run();
            }
            if (writePlayCount) {
                Stmt set(m_db, "UPDATE tracks SET play_count = ?, last_played_at = ? WHERE id = ?");
                set.bind(1, track.playCount);
                set.bind(2, lastPlayed);
                set.bindInt64(3, id);
                set.run();
            }
            if (!artworkIntake.sha.empty() && storedArtworkSha.empty()) {
                Stmt set(m_db, "UPDATE tracks SET artwork_sha = ?, artwork_extension = ? WHERE id = ?");
                set.bind(1, artworkIntake.sha);
                set.bind(2, artworkIntake.extension);
                set.bindInt64(3, id);
                set.run();
            }
        }

        if (writeCues) {
            Stmt clear(m_db, "DELETE FROM cues WHERE track_id = ?");
            clear.bindInt64(1, id);
            clear.run();
            Stmt insert(m_db, R"sql(
                INSERT INTO cues (track_id, kind, hot_number, position_ms, color, comment, is_loop, loop_end_ms)
                VALUES (?,?,?,?,?,?,?,?)
            )sql");
            for (const auto &cue : track.cues) {
                insert.reset();
                insert.bindInt64(1, id);
                insert.bind(2, kindToText(cue.kind));
                insert.bind(3, cue.hotCueNumber);
                insert.bind(4, cue.positionMs);
                insert.bind(5, cue.color);
                insert.bind(6, cue.comment);
                insert.bind(7, cue.isLoop ? 1 : 0);
                insert.bind(8, cue.loopEndMs);
                insert.run();
                summary.cuesStored++;
            }
        }

        // Playlist membership is authored but not conflicting: it is a
        // set, the freshest reading of it is the right one, and there is
        // no case where a stale membership is worth keeping over a
        // current one. Only replaced when the incoming reading has any,
        // so a catalog that does not report playlists cannot erase what
        // another one did.
        if (!track.playlists.empty()) {
            Stmt clear(m_db, "DELETE FROM playlists WHERE track_id = ?");
            clear.bindInt64(1, id);
            clear.run();
            Stmt insert(m_db, "INSERT INTO playlists (track_id, name, position) VALUES (?,?,?)");
            for (const auto &playlist : track.playlists) {
                insert.reset();
                insert.bindInt64(1, id);
                insert.bind(2, playlist.name);
                insert.bind(3, playlist.position);
                insert.run();
            }
        }

        if (exists) {
            // Mutually exclusive, so the four counts add up to the
            // tracks seen. A track that both took something new and kept
            // something old counts as updated: "skipped" reads as
            // "nothing of yours was replaced", and it has to stay true.
            const bool wroteSomething = writeCues || writeRating || writeComment || writePlayCount;
            const bool keptSomething = (cuesConflict && !writeCues) || (ratingConflict && !writeRating) ||
                (commentConflict && !writeComment) || (playCountConflict && !writePlayCount);
            if (wroteSomething) {
                summary.tracksUpdated++;
            } else if (keptSomething) {
                summary.tracksSkipped++;
            } else {
                summary.tracksUnchanged++;
            }
        }
    }

    exec(m_db, "COMMIT");
    guard.committed = true;
    progress.finish();
    return summary;
}

// ---- reading --------------------------------------------------------

std::vector<CuePoint> MetadataStore::cuesFor(std::int64_t trackId)
{
    std::vector<CuePoint> cues;
    Stmt stmt(m_db,
              "SELECT kind, hot_number, position_ms, color, comment, is_loop, loop_end_ms "
              "FROM cues WHERE track_id = ? ORDER BY position_ms");
    stmt.bindInt64(1, trackId);
    while (stmt.step()) {
        CuePoint cue;
        cue.kind = kindFromText(stmt.columnText(0));
        cue.hotCueNumber = stmt.columnInt(1);
        cue.positionMs = stmt.columnDouble(2);
        cue.color = stmt.columnText(3);
        cue.comment = stmt.columnText(4);
        cue.isLoop = stmt.columnInt(5) != 0;
        cue.loopEndMs = stmt.columnDouble(6);
        cues.push_back(std::move(cue));
    }
    return cues;
}

std::vector<PlaylistMembership> MetadataStore::playlistsFor(std::int64_t trackId)
{
    std::vector<PlaylistMembership> playlists;
    Stmt stmt(m_db, "SELECT name, position FROM playlists WHERE track_id = ? ORDER BY name");
    stmt.bindInt64(1, trackId);
    while (stmt.step()) {
        PlaylistMembership membership;
        membership.name = stmt.columnText(0);
        membership.position = stmt.columnInt(1);
        playlists.push_back(std::move(membership));
    }
    return playlists;
}

int MetadataStore::removeTracks(const std::vector<std::int64_t> &trackIds)
{
    if (trackIds.empty()) {
        return 0;
    }
    exec(m_db, "BEGIN IMMEDIATE");
    struct RollbackGuard
    {
        sqlite3 *db;
        bool committed = false;
        ~RollbackGuard()
        {
            if (!committed) {
                sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            }
        }
    } guard{m_db};

    int removed = 0;
    for (const std::int64_t id : trackIds) {
        // The cues and playlist rows go with it: both reference
        // tracks(id) ON DELETE CASCADE, and PRAGMA foreign_keys is on
        // for every connection this store opens, so one statement is
        // genuinely enough. Deleting the track row alone with the
        // pragma off would leave cues nothing owns.
        Stmt remove(m_db, "DELETE FROM tracks WHERE id = ?");
        remove.bindInt64(1, id);
        remove.run();
        removed += sqlite3_changes(m_db);
    }
    exec(m_db, "COMMIT");
    guard.committed = true;
    return removed;
}

std::map<std::int64_t, std::string> MetadataStore::stickLabelsByTrackId()
{
    std::map<std::int64_t, std::string> labels;
    Stmt stmt(m_db, "SELECT id, stick_label FROM tracks WHERE stick_label <> ''");
    while (stmt.step()) {
        labels.emplace(stmt.columnInt64(0), stmt.columnText(1));
    }
    return labels;
}

std::vector<Track> MetadataStore::readAll()
{
    std::vector<Track> tracks;
    {
        Stmt stmt(m_db, R"sql(
            SELECT id, relative_path, filename, title, artist, duration_seconds, bpm,
                   music_key, rating, comment, play_count, updated_at
            FROM tracks ORDER BY id
        )sql");
        while (stmt.step()) {
            Track track;
            track.sourceId = std::to_string(stmt.columnInt64(0));
            track.format = "metadata-store";
            // filePath is left empty on purpose, and the relative path
            // this row does keep is not put in it.
            //
            // matchTracks() treats an exact path match as decisive and
            // needs no other evidence, which is right when both sides
            // are reading one stick and wrong here: this store outlives
            // the stick. A stored path is stick-relative and a stick's
            // is absolute, so the branch could only ever fire by
            // accident -- and an accidental decisive match is a worse
            // failure than no match at all. Empty means the store is
            // always matched the way it is meant to be: on title, artist
            // and length.
            track.filename = stmt.columnText(2);
            track.title = stmt.columnText(3);
            track.artist = stmt.columnText(4);
            track.durationSeconds = stmt.columnDouble(5);
            track.bpm = stmt.columnDouble(6);
            track.key = stmt.columnText(7);
            track.rating = stmt.columnOptionalInt(8);
            track.comment = stmt.columnText(9);
            track.playCount = stmt.columnOptionalInt(10);
            // Exact, per row, because this store wrote the row. The
            // stick side of the same comparison has only its catalogs'
            // mtime to offer.
            track.metadataModifiedAt = epochFromIsoTimestamp(stmt.columnText(11));
            tracks.push_back(std::move(track));
        }
    }
    for (auto &track : tracks) {
        track.cues = cuesFor(std::stoll(track.sourceId));
    }
    return tracks;
}

int MetadataStore::trackCount(const std::string &search)
{
    const std::string pattern = "%" + lowercased(search) + "%";
    Stmt stmt(m_db,
              "SELECT COUNT(*) FROM tracks WHERE ? = '' OR lower(title) LIKE ? "
              "OR lower(artist) LIKE ? OR lower(filename) LIKE ?");
    stmt.bind(1, search);
    stmt.bind(2, pattern);
    stmt.bind(3, pattern);
    stmt.bind(4, pattern);
    return stmt.step() ? stmt.columnInt(0) : 0;
}

std::vector<StoredTrack> MetadataStore::browse(const std::string &search, int limit, int offset)
{
    std::vector<StoredTrack> rows;
    const std::string pattern = "%" + lowercased(search) + "%";
    Stmt stmt(m_db, R"sql(
        SELECT t.id, t.relative_path, t.filename, t.title, t.artist, t.duration_seconds,
               t.bpm, t.music_key, t.rating, t.comment, t.play_count, t.last_played_at,
               t.artwork_sha, t.artwork_extension, t.stick_label, t.source_format, t.updated_at,
               (SELECT COUNT(*) FROM cues c WHERE c.track_id = t.id),
               (SELECT COUNT(*) FROM playlists p WHERE p.track_id = t.id)
        FROM tracks t
        WHERE ? = '' OR lower(t.title) LIKE ? OR lower(t.artist) LIKE ? OR lower(t.filename) LIKE ?
        ORDER BY lower(t.artist), lower(t.title), t.id
        LIMIT ? OFFSET ?
    )sql");
    stmt.bind(1, search);
    stmt.bind(2, pattern);
    stmt.bind(3, pattern);
    stmt.bind(4, pattern);
    stmt.bind(5, limit);
    stmt.bind(6, offset);
    while (stmt.step()) {
        StoredTrack row;
        row.id = stmt.columnInt64(0);
        row.relativePath = stmt.columnText(1);
        row.filename = stmt.columnText(2);
        row.title = stmt.columnText(3);
        row.artist = stmt.columnText(4);
        row.durationSeconds = stmt.columnDouble(5);
        row.bpm = stmt.columnDouble(6);
        row.key = stmt.columnText(7);
        row.rating = stmt.columnOptionalInt(8);
        row.comment = stmt.columnText(9);
        row.playCount = stmt.columnOptionalInt(10);
        row.lastPlayedAt = stmt.columnText(11);
        const std::string sha = stmt.columnText(12);
        if (!sha.empty()) {
            row.artworkPath = (artworkDir() / (sha + stmt.columnText(13))).string();
        }
        row.stickLabel = stmt.columnText(14);
        row.sourceFormat = stmt.columnText(15);
        row.updatedAt = stmt.columnText(16);
        row.cueCount = stmt.columnInt(17);
        row.playlistCount = stmt.columnInt(18);
        rows.push_back(std::move(row));
    }
    return rows;
}

}  // namespace seabass::infrastructure::local
