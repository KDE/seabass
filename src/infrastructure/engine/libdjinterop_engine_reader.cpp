// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/libdjinterop_engine_reader.hpp"
#include "infrastructure/engine/engine_artwork.hpp"
#include "infrastructure/engine/engine_pending_journals.hpp"
#include "infrastructure/engine/engine_sqlite.hpp"
#include "infrastructure/hashing/sha1.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/paths/seabass_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string_view>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include <sqlite3.h>

#include <djinterop/djinterop.hpp>
#include <djinterop/track_snapshot.hpp>

namespace seabass::infrastructure::engine
{

namespace application = seabass::application;
namespace domain = seabass::domain;

// alpha == 0 is djinterop::pad_color's own default-constructed value (see
// its own doc comment: "Construct a pad_color with a default black
// color"), and the blob decoder (quick_cues_blob.cpp) reads whatever
// alpha byte is actually stored -- it isn't synthesized by libdjinterop.
// Confirmed on real data this session: hot cues never explicitly colored
// on real hardware come through with alpha == 0, previously rendered as
// the misleading "#000000" (indistinguishable from a genuinely
// black-colored cue) instead of "no color at all". That false distinction
// from rekordbox's own "no color" representation (color_id == 0, see
// kaitai_rekordbox_reader.cpp's cueColor()) was making cueSetsEqual()
// treat identical, uncolored cues from the two formats as a real
// conflict -- confirmed as the actual cause of a full batch of "genuine"
// cross-source sync conflicts that weren't genuine at all.
std::string colorHex(const djinterop::pad_color &c)
{
    if (c.a == 0) {
        return "";
    }
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
    return buf;
}

namespace
{

// Some individual fields on some tracks (observed: sample_rate() on a
// track just re-cued on real Denon hardware) throw a decode error from
// libdjinterop even when the rest of the track, including hot_cues(),
// reads fine. Isolating each field like this means one flaky field never
// costs us the whole track, which a single try/catch around everything
// used to do.
template<typename T, typename Fn>
T safeGet(application::ProgressReporter &progress, int64_t trackId, const char *fieldName, Fn &&fn, T fallback = T{})
{
    try {
        return fn();
    } catch (const std::exception &e) {
        progress.warn("track id=" + std::to_string(trackId) + ": " + fieldName + " unreadable (" + e.what() + ")");
        return fallback;
    }
}

// Recursively walks a playlist tree, recording each track's full playlist
// path(s) (e.g. "Techno/Peak Time"). Best-effort: any failure here just
// leaves playlists empty rather than failing the whole scan, since
// membership is supplementary information, not core track data.
void collectPlaylistMemberships(const djinterop::playlist &pl, const std::string &pathPrefix,
                                 std::unordered_map<int64_t, std::vector<domain::PlaylistMembership>> &membership)
{
    std::string path = pathPrefix.empty() ? pl.name() : pathPrefix + "/" + pl.name();
    int position = 0;
    for (const auto &tr : pl.tracks()) {
        membership[tr.id()].push_back(domain::PlaylistMembership{path, position++});
    }
    for (const auto &child : pl.children()) {
        collectPlaylistMemberships(child, path, membership);
    }
}

// Track id -> best-effort resolved artwork file path, read directly via
// plain SQLite3 rather than libdjinterop's own public API - track.hpp
// exposes no way to get at album art at all (djinterop::album_art exists
// as a type but is an acknowledged stub, "TODO - implement rest of
// album_art class", and database.hpp has no method returning one), even
// though the underlying schema has real, resolvable data: every Track row
// has an albumArtId, and the AlbumArt row it names says where the image is.
// Two of its spellings are read here. An imported one's hash is a URI like
// "image://fileart//media/<label>/PIONEER/Artwork/00001/a5_m.jpg" pointing
// at a real external JPEG file on the stick; <label> is whatever volume
// label the *original* Denon hardware mounted the stick under, not
// necessarily this machine's, so this anchors on the stable
// "PIONEER/Artwork/..." suffix instead of trying to match the volume
// label. An older library keeps the image itself in AlbumArt.albumArt;
// see databaseArtworkFile(). seabass_core already links plain SQLite3
// directly for LocalCueStore, so this doesn't add a new dependency; opened
// as a second, independent, read-only connection to the same m.db
// djinterop::engine::load_database() above already has open, never
// written through.
// A snapshot assembled from the per-field getters, for a row whose
// snapshot() threw. Each getter is its own statement and its own
// try/catch: a field that cannot be read is left unset and named in a
// warning, and the rest of the track reads as usual.
djinterop::track_snapshot snapshotFromGetters(application::ProgressReporter &progress, const djinterop::track &tr)
{
    djinterop::track_snapshot snap;
    const int64_t id = tr.id();
    const auto field = [&](const char *name, auto &&read) {
        try {
            read();
        } catch (const std::exception &e) {
            progress.warn("track id=" + std::to_string(id) + ": " + name + " unreadable (" + e.what() + ")");
        }
    };
    field("title", [&] { snap.title = tr.title(); });
    field("artist", [&] { snap.artist = tr.artist(); });
    field("album", [&] { snap.album = tr.album(); });
    field("relative_path", [&] { snap.relative_path = tr.relative_path(); });
    field("bpm", [&] { snap.bpm = tr.bpm(); });
    field("bitrate", [&] { snap.bitrate = tr.bitrate(); });
    field("key", [&] { snap.key = tr.key(); });
    field("duration", [&] { snap.duration = tr.duration(); });
    field("last_played_at", [&] { snap.last_played_at = tr.last_played_at(); });
    field("rating", [&] { snap.rating = tr.rating(); });
    field("comment", [&] { snap.comment = tr.comment(); });
    field("sample_rate", [&] { snap.sample_rate = tr.sample_rate(); });
    field("hot_cues", [&] { snap.hot_cues = tr.hot_cues(); });
    field("loops", [&] { snap.loops = tr.loops(); });
    field("main_cue", [&] { snap.main_cue = tr.main_cue(); });
    return snap;
}

// The image an AlbumArt row keeps in the database, as a file on this
// computer, since everything that shows a cover takes a path. Written once
// into `libraryDirectory`, which belongs to this one library (its
// Information uuid), so a later scan finds it with a stat and never reads
// the image again: under the row's hex hash and the image's length, or,
// for a hash that is no hex (an imported path, or none), under the row, a
// checksum of that hash and the length. The length is in the name because
// the uuid does not settle it: a clone of a stick keeps its uuid, and
// Engine's hash is not a checksum of the bytes. Without a directory of the
// library's own (no uuid) the name is a checksum of the bytes, which costs
// a read each time. Empty when the row holds nothing a player could draw;
// that is known from the image's first bytes, and remembered for the run.
std::string databaseArtworkFile(sqlite3 *db, int &failures, std::int64_t albumArtId,
                                const std::string &hash, std::int64_t length,
                                const std::filesystem::path &libraryDirectory)
{
    const bool hexHash = !libraryDirectory.empty() && !hash.empty() && hash.size() <= 64
        && std::all_of(hash.begin(), hash.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
    const std::filesystem::path directory =
        libraryDirectory.empty() ? paths::localEngineArtworkDir() / "by-content" : libraryDirectory;
    std::string knownName;
    if (hexHash) {
        knownName = hash + "-" + std::to_string(length);
    } else if (!libraryDirectory.empty()) {
        knownName = "row-" + std::to_string(albumArtId) + "-"
            + hashing::toHex(hashing::Sha256::of(std::string_view(hash))).substr(0, 16) + "-" + std::to_string(length);
    }
    std::error_code ec;
    if (!knownName.empty()) {
        for (const char *extension : {".jpg", ".png"}) {
            const std::filesystem::path known = directory / pathFromUtf8(knownName + extension);
            if (std::filesystem::is_regular_file(known, ec)
                && std::filesystem::file_size(known, ec) == static_cast<std::uintmax_t>(length) && !ec) {
                return pathToUtf8(known);
            }
        }
    }
    // Bytes no player draws are told by their first bytes, and not asked
    // about again in this run.
    static std::mutex notImagesLock;
    static std::set<std::string> notImages;
    const std::string rowKey = pathToUtf8(directory) + "|" + std::to_string(albumArtId) + "|" + std::to_string(length)
        + "|" + hash;
    {
        const std::lock_guard<std::mutex> guard(notImagesLock);
        if (notImages.count(rowKey) != 0) {
            return {};
        }
    }
    // Its own blob handle, closed at once: none may stay open while copies
    // are written, since an open one holds the database.
    const std::optional<std::string> head = albumArtImageHead(db, albumArtId);
    if (!head) {
        failures++;  // not read: counted, and nothing concluded about it
        return {};
    }
    if (extensionForImage(*head).empty()) {
        const std::lock_guard<std::mutex> guard(notImagesLock);
        notImages.insert(rowKey);
        return {};
    }
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT albumArt FROM AlbumArt WHERE id = ?", -1, &stmt, nullptr) != SQLITE_OK) {
        failures++;
        return {};
    }
    sqlite3_bind_int64(stmt, 1, albumArtId);
    std::string bytes;
    const int step = sqlite3_step(stmt);
    if (step != SQLITE_ROW && step != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        failures++;
        return {};
    }
    if (step == SQLITE_ROW) {
        if (const void *blob = sqlite3_column_blob(stmt, 0)) {
            bytes.assign(static_cast<const char *>(blob), static_cast<size_t>(sqlite3_column_bytes(stmt, 0)));
        }
    }
    sqlite3_finalize(stmt);
    const std::string extension = extensionForImage(bytes);
    if (extension.empty()) {
        return {};
    }
    const std::string name = !knownName.empty() ? knownName
                                                : hashing::toHex(hashing::Sha256::of(std::string_view(bytes)));
    const std::filesystem::path file = directory / pathFromUtf8(name + extension);
    std::filesystem::create_directories(directory, ec);
    const auto whole = [&file, &bytes] {
        std::error_code sizeError;
        return std::filesystem::is_regular_file(file, sizeError)
            && std::filesystem::file_size(file, sizeError) == bytes.size() && !sizeError;
    };
    if (!whole()) {
        // A temporary name of its own and a rename: two reads (the prefetch
        // and a page, or the app and the command line) can write the same
        // cover at once, and neither may rename the other's half-written
        // file into place. Only for this cache on this computer, never a
        // stick, so a write interrupted here leaves nothing on a stick.
        static const std::uint64_t process = (std::uint64_t{std::random_device{}()} << 32) ^ std::random_device{}();
        static std::atomic<std::uint64_t> writes{0};
        std::filesystem::path part = file;
        part += "." + std::to_string(process) + "-" + std::to_string(writes.fetch_add(1)) + ".tmp";
        {
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        std::filesystem::rename(part, file, ec);
        std::filesystem::remove(part, ec);
    }
    if (!whole()) {
        failures++;  // not written, not renamed into place, or not whole
        return {};
    }
    return pathToUtf8(file);
}

std::unordered_map<int64_t, std::string> readArtworkPaths(const std::string &engineLibraryPath)
{
    std::unordered_map<int64_t, std::string> result;
    const std::filesystem::path root = pathFromUtf8(engineLibraryPath);
    const std::filesystem::path stickRoot = root.parent_path();
    std::string dbPath = pathToUtf8(root / "Database2" / "m.db");

    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return result;
    }

    sqlite3_stmt *stmt = nullptr;
    const char *sql =
        "SELECT t.id, a.hash FROM Track t JOIN AlbumArt a ON a.id = t.albumArtId "
        "WHERE t.albumArtId IS NOT NULL AND t.albumArtId != 0";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return result;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t trackId = sqlite3_column_int64(stmt, 0);
        const unsigned char *hashText = sqlite3_column_text(stmt, 1);
        std::string hash = hashText ? reinterpret_cast<const char *>(hashText) : std::string();
        auto pos = hash.find("PIONEER/Artwork");
        if (pos == std::string::npos) {
            continue;
        }
        // make_preferred(): hash.substr(pos) carries its own forward
        // slashes ("PIONEER/Artwork/...") appended as one path component
        // in a single operator/ call, so fs::path preserves them as
        // literal characters rather than re-splitting into components --
        // pathToUtf8() would otherwise mix them with the native separator
        // from the stickRoot join on Windows. Same bug/fix as
        // OneLibraryReader's artworkPath (onelibrary_reader.cpp), found
        // via a real Windows test failure there.
        //
        // hash is a raw Engine sqlite column, so on Windows pathFromUtf8()
        // can throw over bytes that are not valid UTF-8 (see path_key.cpp's
        // own doc comment on the same risk). Caught per
        // row, not just by readAll()'s outer try/catch around this whole
        // function: that one would otherwise lose every OTHER track's
        // artwork too, not just this row's.
        //
        // Not checked for existence: the catalog says which artwork a track
        // has, and whether the file is still there is for the consumer that
        // draws or audits it to find out. A stat per artwork here was
        // ~1500 stats on a real stick, paid by every page that wanted a
        // title. Library Health's artwork audit reads its own rows.
        try {
            std::filesystem::path candidate = (stickRoot / pathFromUtf8(hash.substr(pos))).make_preferred();
            result[trackId] = pathToUtf8(candidate);
        } catch (const std::exception &) {
            // Best-effort, same as the rest of this loop's field reads:
            // one unreadable artwork path is not worth losing the scan.
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}

// Track id -> cover, for the art that costs more than the catalog to find:
// an image kept in the row, written out to this computer, and an image
// file under Artwork/ named by the row's blob hash, looked for on the
// stick. The artwork stage of a progressive read, so the track list never
// waits for either.
// Which stick, or failing that which location, a library's local copies
// belong to. Clones of one library share its uuid, its hashes and maybe an
// image's length while holding different bytes, so the uuid alone does not
// settle whose copy a file is. A filesystem identity survives the stick
// being mounted elsewhere; the location is the fallback, which costs an
// extraction again when the same stick comes back at another path.
std::string localCopiesKey(const std::string &engineLibraryPath, const std::string &volumeIdentity)
{
    const bool usable = !volumeIdentity.empty() && volumeIdentity.size() <= 64
        && std::all_of(volumeIdentity.begin(), volumeIdentity.end(),
                       [](unsigned char c) { return std::isalnum(c) != 0 || c == '-'; });
    if (usable) {
        return "volume-" + volumeIdentity;
    }
    std::error_code ec;
    const std::filesystem::path location = std::filesystem::weakly_canonical(pathFromUtf8(engineLibraryPath), ec);
    const std::string where = pathToUtf8(ec ? pathFromUtf8(engineLibraryPath) : location);
    return "at-" + hashing::toHex(hashing::Sha256::of(std::string_view(where))).substr(0, 16);
}

std::unordered_map<int64_t, std::string> readStoredArtwork(const std::string &engineLibraryPath,
                                                           const std::string &volumeIdentity,
                                                           const application::CancellationToken &cancel,
                                                           int &failures)
{
    std::unordered_map<int64_t, std::string> result;
    const std::filesystem::path dbFile = pathFromUtf8(engineLibraryPath) / "Database2" / "m.db";
    const std::string dbPath = pathToUtf8(dbFile);
    // An Engine 1.x library has no Database2/m.db and nothing here to read.
    // Anything else that fails is said, not taken for a library without
    // covers.
    std::error_code existsError;
    if (!std::filesystem::exists(dbFile, existsError) && !existsError) {
        return result;
    }
    sqlite3 *db = nullptr;
    const auto fail = [&db, &dbPath](const std::string &what) {
        const std::string message = db ? sqlite3_errmsg(db) : "out of memory";
        if (db) {
            sqlite3_close(db);
        }
        throw std::runtime_error(what + " " + dbPath + ": " + message);
    };
    if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        fail("could not open");
    }
    // A save committing holds the database for a moment: waited out, the
    // way the other connections to it are, not reported as a failed read.
    sqlite3_busy_timeout(db, 5000);
    const std::string artworkDirectory = pathToUtf8(pathFromUtf8(engineLibraryPath) / "Artwork");
    // The library's own directory: its Information uuid, and the stick it
    // is on, since a clone keeps the uuid (localCopiesKey()).
    std::filesystem::path libraryDirectory;
    {
        sqlite3_stmt *information = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT uuid FROM Information ORDER BY id LIMIT 1", -1, &information, nullptr)
                == SQLITE_OK
            && sqlite3_step(information) == SQLITE_ROW) {
            const unsigned char *text = sqlite3_column_text(information, 0);
            const std::string uuid = text ? reinterpret_cast<const char *>(text) : std::string();
            if (!uuid.empty() && uuid.size() <= 64
                && std::all_of(uuid.begin(), uuid.end(),
                               [](unsigned char c) { return std::isalnum(c) != 0 || c == '-'; })) {
                libraryDirectory = paths::localEngineArtworkDir() / uuid / localCopiesKey(engineLibraryPath, volumeIdentity);
            }
        }
        sqlite3_finalize(information);
    }
    // A schema without the image column keeps no images in the database.
    std::string imageLengthSql = "0";
    try {
        if (hasColumn(db, "AlbumArt", "albumArt")) {
            imageLengthSql = byteLengthSql("a.albumArt");
        }
    } catch (const std::exception &) {
        fail("could not list the AlbumArt columns in");
    }
    sqlite3_stmt *stmt = nullptr;
    const std::string sql = "SELECT t.id, a.hash, a.id, " + imageLengthSql
        + " FROM Track t JOIN AlbumArt a ON a.id = t.albumArtId "
          "WHERE t.albumArtId IS NOT NULL AND t.albumArtId != 0 AND ("
        + imageLengthSql + " > 0 OR (typeof(a.hash) = 'blob' AND length(a.hash) > 0))";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        fail("could not read AlbumArt in");
    }
    // The rows first, and the statement finished, before any image is
    // read or written out: a read holds a lock on the database for as long
    // as its statement runs, and writing out a library's covers can take
    // longer than a save waits for one.
    struct StoredRow
    {
        int64_t trackId = 0;
        int64_t albumArtId = 0;
        bool blobHash = false;
        std::string hash;
        std::int64_t imageLength = 0;
    };
    std::vector<StoredRow> rows;
    int step;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (cancel.cancelled()) {
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            throw application::OperationCancelled();
        }
        StoredRow row;
        row.trackId = sqlite3_column_int64(stmt, 0);
        row.albumArtId = sqlite3_column_int64(stmt, 2);
        row.imageLength = sqlite3_column_int64(stmt, 3);
        row.blobHash = sqlite3_column_type(stmt, 1) == SQLITE_BLOB;
        if (const void *hash = sqlite3_column_blob(stmt, 1)) {
            row.hash.assign(static_cast<const char *>(hash), static_cast<size_t>(sqlite3_column_bytes(stmt, 1)));
        }
        rows.push_back(std::move(row));
    }
    // Anything but the end of the rows (a lock held too long, a read
    // error) leaves covers out, and is said rather than returned as all.
    if (step != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        fail("stopped reading AlbumArt in");
    }
    sqlite3_finalize(stmt);

    // Per AlbumArt row, since many tracks share one, each image in a short
    // statement of its own.
    std::unordered_map<int64_t, std::string> fileByRow;
    for (const StoredRow &row : rows) {
        // Per row: a stop lands within one image, with the database let go.
        if (cancel.cancelled()) {
            sqlite3_close(db);
            throw application::OperationCancelled();
        }
        auto known = fileByRow.find(row.albumArtId);
        if (known == fileByRow.end()) {
            std::string file;
            try {
                // An image in the row wins; bytes there that are no image
                // leave the art to whatever the hash names.
                if (row.imageLength > 0 && !row.blobHash) {
                    file = databaseArtworkFile(db, failures, row.albumArtId, row.hash, row.imageLength, libraryDirectory);
                } else if (row.imageLength > 0) {
                    // Named by the blob hash in hex, so a later read finds
                    // the copy with a stat, as for a text hash.
                    const std::string hex = hashing::toHex(std::span<const std::uint8_t>(
                        reinterpret_cast<const std::uint8_t *>(row.hash.data()), row.hash.size()));
                    file = databaseArtworkFile(db, failures, row.albumArtId, hex, row.imageLength, libraryDirectory);
                }
                if (file.empty() && row.blobHash && !std::string_view(row.hash).starts_with("image://")) {
                    const std::span<const std::uint8_t> hash(reinterpret_cast<const std::uint8_t *>(row.hash.data()),
                                                             row.hash.size());
                    // A stat per cover, as the catalog's last stage always
                    // cost: this stage runs again inside saves, and opening
                    // every file each time was a read per cover per save.
                    // An empty or broken file shows as the fallback art, and
                    // the audit reports it.
                    file = cachedArtworkFile(artworkDirectory, hash, nullptr, false);
                }
            } catch (const std::exception &) {
                failures++;  // one unreadable image is not worth the others, but it is counted
            }
            known = fileByRow.emplace(row.albumArtId, std::move(file)).first;
        }
        if (!known->second.empty()) {
            result[row.trackId] = known->second;
        }
    }
    sqlite3_close(db);
    return result;
}

// Track id -> streaming source (e.g. "TIDAL"), same raw-SQLite workaround
// as readArtworkPaths() above and for the same reason: libdjinterop's
// public API never exposes Track.streamingSource/uri at all (open TODOs
// in the library's own headers acknowledge this). A track with this set
// has no real local file. Its `path` column points at a streaming-
// cache location on the *computer* that manages playback, never at
// anything present on this stick, so callers must never treat it as an
// ordinary local track (play it, merge it, sync it, clean it up).
std::unordered_map<int64_t, std::string> readStreamingSources(const std::string &engineLibraryPath)
{
    std::unordered_map<int64_t, std::string> result;
    std::string dbPath = pathToUtf8(pathFromUtf8(engineLibraryPath) / "Database2" / "m.db");

    // Two outcomes that look alike and are not. An Engine 1.x library has no
    // Database2/m.db, and a 2.x one older than schema 2.18 no streamingSource
    // column, because neither has streaming tracks: that is an empty map, and
    // nothing to warn about. Anything else going wrong means
    // streaming tracks cannot be told apart, and a streaming track read as a
    // local one looks like a broken file -- which Library Health offers to
    // repair by deleting the row. So that throws, and the read fails.
    std::error_code existsError;
    if (!std::filesystem::exists(pathFromUtf8(dbPath), existsError) && !existsError) {
        return result;
    }

    sqlite3 *db = nullptr;
    const auto fail = [&db, &dbPath](const std::string &what) {
        std::string message = db ? sqlite3_errmsg(db) : "out of memory";
        if (db) {
            sqlite3_close(db);
        }
        throw std::runtime_error("cannot tell streaming tracks apart in " + dbPath + ": " + what + ": " + message);
    };
    if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        fail("open");
    }

    bool hasColumn = false;
    {
        sqlite3_stmt *columns = nullptr;
        if (sqlite3_prepare_v2(db, "PRAGMA table_info(Track)", -1, &columns, nullptr) != SQLITE_OK) {
            fail("list Track columns");
        }
        int step;
        while ((step = sqlite3_step(columns)) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(columns, 1);
            if (name && std::string(reinterpret_cast<const char *>(name)) == "streamingSource") {
                hasColumn = true;
            }
        }
        sqlite3_finalize(columns);
        if (step != SQLITE_DONE) {
            fail("list Track columns");
        }
    }
    if (!hasColumn) {
        sqlite3_close(db);
        return result;
    }

    sqlite3_stmt *stmt = nullptr;
    const char *sql = "SELECT id, streamingSource FROM Track WHERE streamingSource IS NOT NULL AND streamingSource != ''";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        fail("query");
    }

    int step;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        int64_t trackId = sqlite3_column_int64(stmt, 0);
        const unsigned char *sourceText = sqlite3_column_text(stmt, 1);
        if (!sourceText) {
            continue;
        }
        result[trackId] = reinterpret_cast<const char *>(sourceText);
    }
    sqlite3_finalize(stmt);
    if (step != SQLITE_DONE) {
        fail("read");
    }
    sqlite3_close(db);
    return result;
}

// Track id -> Track.lastEditTime, in seconds since the epoch. Raw SQLite
// for the same reason as the two readers above: libdjinterop's high-level
// track API does not expose the column (only its v2 table row type does).
//
// This is the per-track clock Sync resolves a hot cue conflict with.
// Without it the only date was m.db's mtime, which moves for every track
// the moment any one is edited, so it could not tell which side of one
// particular track was newer -- and a sync is exactly what makes m.db the
// newest file on the stick. A value of 0 or below is "unknown" and left
// out, which makes Sync fall back to the catalog dates for that track.
std::unordered_map<int64_t, std::int64_t> readLastEditTimes(const std::string &engineLibraryPath)
{
    std::unordered_map<int64_t, std::int64_t> result;
    std::string dbPath = pathToUtf8(pathFromUtf8(engineLibraryPath) / "Database2" / "m.db");

    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return result;
    }

    sqlite3_stmt *stmt = nullptr;
    const char *sql = "SELECT id, lastEditTime FROM Track WHERE lastEditTime IS NOT NULL AND lastEditTime > 0";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        // An older schema without the column: no per-track clock, not an
        // error. Sync falls back to the catalog dates.
        sqlite3_close(db);
        return result;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        result[sqlite3_column_int64(stmt, 0)] = sqlite3_column_int64(stmt, 1);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}

}  // namespace

LibdjinteropEngineReader::LibdjinteropEngineReader(std::string engineLibraryPath)
    : m_engineLibraryPath(std::move(engineLibraryPath))
{
}

std::vector<domain::Track> LibdjinteropEngineReader::readAll()
{
    return readTracks();
}

void LibdjinteropEngineReader::fillArtwork(std::vector<domain::Track> &tracks)
{
    std::unordered_map<int64_t, std::string> stored;
    int failures = 0;
    try {
        // Its own read-only open, maybe long after readTracks(): a journal
        // a pulled stick left is recovered first here too. One that cannot
        // be costs the covers, not the rest of the stage this runs in.
        recoverEnginePendingJournals(m_engineLibraryPath);
        stored = readStoredArtwork(m_engineLibraryPath, m_volumeIdentity, m_cancel, failures);
    } catch (const application::OperationCancelled &) {
        throw;
    } catch (const std::exception &e) {
        m_progress->warn(std::string("could not read album art: ") + e.what());
        return;
    }
    for (auto &track : tracks) {
        m_cancel.throwIfCancelled();
        try {
            const auto found = stored.find(std::stoll(track.sourceId));
            if (found != stored.end()) {
                track.artworkPath = found->second;
            }
        } catch (const std::logic_error &) {
            // Not a row id: not a track of this catalog.
        }
    }
    if (failures > 0) {
        m_progress->warn(std::to_string(failures) + " covers could not be copied from the Engine database");
    }
}

std::optional<size_t> LibdjinteropEngineReader::countTracks()
{
    const std::string dbPath = pathToUtf8(pathFromUtf8(m_engineLibraryPath) / "Database2" / "m.db");
    sqlite3 *handle = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (handle != nullptr) {
            sqlite3_close(handle);
        }
        return std::nullopt;
    }
    std::optional<size_t> count;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(handle, "SELECT count(*) FROM Track;", -1, &stmt, nullptr) == SQLITE_OK
        && sqlite3_step(stmt) == SQLITE_ROW) {
        count = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    sqlite3_close(handle);
    return count;
}

std::vector<domain::Track> LibdjinteropEngineReader::readTracks()
{
    // Before anything opens a database here, database_exists() included:
    // that one opens m.db read-write, and SQLite rolled a journal a pulled
    // stick left behind back right there, silently, before a copy of it
    // was kept (rig check F6, 2026-09-28). The read-only opens below
    // (artwork, streaming sources, edit times) need it done first too.
    // A database still being written after the wait is not read either:
    // were that writer another recovery that died, database_exists()
    // would roll its journal back with no copy kept.
    const std::string busy = recoverEnginePendingJournals(m_engineLibraryPath, std::chrono::seconds(10));
    if (!busy.empty()) {
        throw std::runtime_error("the Engine Library on this stick is being written right now (" + busy
                                 + " is locked); try again once that has finished");
    }
    if (!djinterop::engine::database_exists(m_engineLibraryPath)) {
        throw std::runtime_error("no Engine Library found at " + m_engineLibraryPath);
    }

    auto db = djinterop::engine::load_database(m_engineLibraryPath);
    auto allTracks = db.tracks();

    std::unordered_map<int64_t, std::vector<domain::PlaylistMembership>> playlistsByTrackId;
    try {
        for (const auto &root : db.root_playlists()) {
            collectPlaylistMemberships(root, "", playlistsByTrackId);
        }
    } catch (const std::exception &e) {
        m_progress->warn(std::string("could not read playlists: ") + e.what());
    }

    std::unordered_map<int64_t, std::string> artworkByTrackId;
    try {
        artworkByTrackId = readArtworkPaths(m_engineLibraryPath);
    } catch (const std::exception &e) {
        m_progress->warn(std::string("could not read album art: ") + e.what());
    }

    std::unordered_map<int64_t, std::string> streamingSourceByTrackId;
    std::unordered_map<int64_t, std::int64_t> lastEditTimeByTrackId;
    // Not caught: see readStreamingSources for why a failure fails the read.
    streamingSourceByTrackId = readStreamingSources(m_engineLibraryPath);
    try {
        lastEditTimeByTrackId = readLastEditTimes(m_engineLibraryPath);
    } catch (const std::exception &e) {
        m_progress->warn(std::string("could not read track edit times: ") + e.what());
    }

    m_progress->start("Scanning Engine tracks", allTracks.size());
    size_t processed = 0;

    std::vector<domain::Track> tracks;
    for (auto &tr : allTracks) {
        int64_t id = tr.id();
        // One read of the row, not one statement per field: each getter
        // on a djinterop::track is its own SELECT, and every statement
        // outside a transaction has SQLite look for a hot journal first,
        // two stats on the stick. Eighteen getters over 1564 tracks were
        // 53,000 such stats and 0.6 s of a warm read. snapshot() reads
        // the row once and hands every field back.
        djinterop::track_snapshot snap;
        try {
            snap = tr.snapshot();
        } catch (const std::exception &e) {
            // snapshot() decodes every blob of the row, waveforms included,
            // and throws for one it cannot (a real stick had a track with a
            // truncated overview waveform). The track is not lost over
            // that: its fields are read one by one instead, the way this
            // reader always did, each on its own so one bad field costs
            // only itself.
            m_progress->warn("track id=" + std::to_string(id) + ": read field by field (" + e.what() + ")");
            snap = snapshotFromGetters(*m_progress, tr);
        }
        domain::Track track;
        track.sourceId = std::to_string(id);
        track.format = "engine";
        track.title = snap.title.value_or("");
        track.artist = snap.artist.value_or("");
        track.album = snap.album.value_or("");
        track.filename = safeGet<std::string>(*m_progress, id, "filename", [&] {
            // The last path component, as libdjinterop's own filename()
            // gives it: the part after the last slash of the relative path.
            const std::string relative = snap.relative_path.value_or("");
            const auto slash = relative.find_last_of('/');
            return slash == std::string::npos ? relative : relative.substr(slash + 1);
        });
        track.filePath = safeGet<std::string>(*m_progress, id, "relative_path", [&] {
            // No relative path names no file. Joined anyway it would name the
            // Engine Library folder itself -- one path shared by every such
            // row, which anything keyed on the file would take for one track.
            const std::string relative = snap.relative_path.value_or("");
            if (relative.empty()) {
                return std::string();
            }
            auto resolved = pathFromUtf8(m_engineLibraryPath) / pathFromUtf8(relative);
            return pathToUtf8(resolved.lexically_normal());
        });
        // No size: Engine does not record one, and a stat per audio file
        // is a stage of its own (application::fillFileSizes).
        track.bpm = snap.bpm.value_or(0.0);
        track.bitrate = snap.bitrate.value_or(0);
        track.key = safeGet<std::string>(*m_progress, id, "key", [&] {
            auto k = snap.key;
            if (!k) {
                return std::string();
            }
            std::ostringstream oss;
            oss << *k;
            return oss.str();
        });
        track.durationSeconds = snap.duration ? snap.duration->count() / 1000.0 : 0.0;
        track.lastPlayedAt = snap.last_played_at;
        track.rating = safeGet<std::optional<int>>(*m_progress, id, "rating", [&] {
            auto r = snap.rating;
            if (!r || *r <= 0) {
                return std::optional<int>{};
            }
            return std::optional<int>{*r / 20};
        });
        track.comment = snap.comment.value_or("");
        auto playlistsIt = playlistsByTrackId.find(id);
        if (playlistsIt != playlistsByTrackId.end()) {
            track.playlists = playlistsIt->second;
        }
        auto artworkIt = artworkByTrackId.find(id);
        if (artworkIt != artworkByTrackId.end()) {
            track.artworkPath = artworkIt->second;
        }
        auto streamingIt = streamingSourceByTrackId.find(id);
        if (streamingIt != streamingSourceByTrackId.end()) {
            track.streamingSource = streamingIt->second;
        }
        auto lastEditIt = lastEditTimeByTrackId.find(id);
        if (lastEditIt != lastEditTimeByTrackId.end()) {
            track.metadataModifiedAt = lastEditIt->second;
        }

        auto sampleRate = snap.sample_rate;
        // Or a stored zero, which real libraries carry: dividing by it
        // gives inf (or NaN at offset 0), and an infinite cue position
        // walks straight past every check that asks whether a cue is near
        // the start.
        if (!sampleRate || *sampleRate <= 0.0) {
            // 44.1kHz is by far the most common sample rate for the
            // compressed audio these libraries hold; falling back to it
            // gives a position that's very likely close to right, instead
            // of treating a raw sample count as if it were milliseconds
            // (which is wrong by roughly a factor of 44).
            sampleRate = 44100.0;
        }
        const auto &hotCues = snap.hot_cues;
        for (size_t i = 0; i < hotCues.size(); ++i) {
            if (!hotCues[i]) {
                continue;
            }
            const auto &hotCue = *hotCues[i];
            if (hotCue.sample_offset < 0.0) {
                continue;  // the same "not set" sentinel as main_cue below
            }
            domain::CuePoint cp;
            cp.kind = domain::CuePoint::Kind::Hot;
            cp.hotCueNumber = static_cast<int>(i) + 1;  // Engine slots are 0-based; rekordbox numbers from 1
            cp.positionMs = hotCue.sample_offset / *sampleRate * 1000.0;
            cp.color = colorHex(hotCue.color);
            cp.comment = hotCue.label;
            track.cues.push_back(std::move(cp));
        }

        // Engine's hot loops live in their own 8-slot array (loops()),
        // separate from hot_cues() above -- indexed the same way, but a
        // genuinely different column family, not a variant of hot_cue. On
        // the hardware a given pad shows either the hot cue or the hot
        // loop for its number depending on pad mode, never both; Seabass
        // itself enforces that one-or-the-other rule when writing (see
        // AddCueController), matching the design this reads back into.
        const auto &loops = snap.loops;
        for (size_t i = 0; i < loops.size(); ++i) {
            if (!loops[i]) {
                continue;
            }
            const auto &loop = *loops[i];
            if (loop.start_sample_offset < 0.0 || loop.end_sample_offset < 0.0) {
                continue;  // ditto: an empty loop slot, not a loop at 0
            }
            domain::CuePoint cp;
            cp.kind = domain::CuePoint::Kind::Hot;
            cp.hotCueNumber = static_cast<int>(i) + 1;
            cp.isLoop = true;
            cp.positionMs = loop.start_sample_offset / *sampleRate * 1000.0;
            cp.loopEndMs = loop.end_sample_offset / *sampleRate * 1000.0;
            cp.color = colorHex(loop.color);
            cp.comment = loop.label;
            track.cues.push_back(std::move(cp));
        }

        // Engine's format has exactly one memory-style cue point (called
        // "Cue" in the app), stored as a plain sample offset with no
        // color/comment, unlike rekordbox's unlimited, independently
        // colored/commented memory cues. Represented here as a single
        // Kind::Memory CuePoint (hotCueNumber 0, matching how rekordbox's
        // own reader marks memory cues) so it can be matched/synced like
        // any other cue; see libdjinterop_engine_cue_writer.cpp for the
        // corresponding (necessarily lossy beyond one cue) write side.
        const auto mainCue = snap.main_cue;
        // A track with no main cue carries -1 as its sample offset, which
        // is libdjinterop's "not set" rather than a position: dividing it
        // by the sample rate made a memory cue a fraction of a
        // millisecond BEFORE the track starts. Every un-cued track on a
        // stick grew one -- 958 of them in one real library -- and they
        // travelled into the metadata store, into restore offers and into
        // every count of how many cues a track has. Anything at or before
        // sample 0 that was not deliberately placed there is not a cue.
        if (mainCue && *mainCue >= 0.0) {
            domain::CuePoint cp;
            cp.kind = domain::CuePoint::Kind::Memory;
            cp.positionMs = *mainCue / *sampleRate * 1000.0;
            track.cues.push_back(std::move(cp));
        }

        tracks.push_back(std::move(track));
        m_progress->tick(++processed);
        m_cancel.throwIfCancelled();
    }

    m_progress->finish();
    return tracks;
}

}  // namespace seabass::infrastructure::engine
