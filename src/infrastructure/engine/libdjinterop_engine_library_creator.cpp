// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/libdjinterop_engine_library_creator.hpp"

#include "infrastructure/engine/engine_artwork.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <span>
#include <string>
#include <vector>

#include <djinterop/djinterop.hpp>
#include <djinterop/engine/engine.hpp>
#include <sqlite3.h>

#include "infrastructure/engine/rekordbox_key_parser.hpp"
#include "infrastructure/hashing/sha256.hpp"
#include "infrastructure/local/sqlite_statement.hpp"
#include "infrastructure/scratch_dir_guard.hpp"

namespace seabass::infrastructure::engine
{

namespace fs = std::filesystem;

namespace
{

constexpr int HotCueSlotCount = 8;

djinterop::engine::engine_schema schemaFor(EngineSchemaGeneration generation)
{
    switch (generation) {
    case EngineSchemaGeneration::V1: return djinterop::engine::latest_v1_schema;
    case EngineSchemaGeneration::V2: return djinterop::engine::latest_v2_schema;
    case EngineSchemaGeneration::V3: return djinterop::engine::latest_v3_schema;
    }
    return djinterop::engine::latest_v2_schema;
}

// A default assumed when a track's own sample rate isn't known -- same
// fallback convention (and same reasoning) as
// LibdjinteropEngineCueWriter's own sample-rate handling.
constexpr double DefaultSampleRate = 44100.0;

// Same conversion LibdjinteropEngineCueWriter::writeHotCues() uses,
// duplicated rather than shared: that writer works against an *existing*
// track by reopening the whole database on every call (fine for the
// handful of stragglers Sync/Library Health/Add Cue write one at a time,
// but reopening the entire database once per track for a bulk import of
// an entire library -- confirmed on real data to be well over a thousand
// tracks -- is what actually caused the reported hang: it isn't an
// infinite loop, just an enormous, avoidable amount of per-track I/O
// against what may well be slow removable media). Setting hot_cues/
// main_cue directly on the snapshot before create_track() writes cues as
// part of the same single insert, no reopen at all.
djinterop::pad_color parseColor(const std::string &color)
{
    if (color.size() == 7 && color[0] == '#') {
        auto hexByte = [&](size_t pos) {
            return static_cast<std::uint8_t>(std::stoi(color.substr(pos, 2), nullptr, 16));
        };
        return djinterop::pad_color{hexByte(1), hexByte(3), hexByte(5), 0xFF};
    }
    return djinterop::pad_color{};
}

// Returns the number of cues actually represented in the snapshot (hot
// cue slots filled, plus one if a memory cue was set) -- not simply
// cues.size(), since Engine has exactly one memory-style cue slot, so
// all but the earliest memory cue are unavoidably lost in this
// direction (see libdjinterop_engine_cue_writer.cpp's own comment).
int applyCuesToSnapshot(djinterop::track_snapshot &snapshot, const std::vector<domain::CuePoint> &cues)
{
    if (cues.empty()) {
        return 0;
    }
    std::vector<std::optional<djinterop::hot_cue>> slots(HotCueSlotCount);
    std::optional<double> earliestMemoryCueMs;
    int hotCuesSet = 0;
    for (const auto &cue : cues) {
        if (cue.kind == domain::CuePoint::Kind::Memory) {
            if (!earliestMemoryCueMs || cue.positionMs < *earliestMemoryCueMs) {
                earliestMemoryCueMs = cue.positionMs;
            }
            continue;
        }
        int slot = cue.hotCueNumber - 1;
        if (slot < 0 || slot >= HotCueSlotCount) {
            continue;
        }
        double sampleOffset = cue.positionMs / 1000.0 * DefaultSampleRate;
        slots[static_cast<size_t>(slot)] = djinterop::hot_cue{cue.comment, sampleOffset, parseColor(cue.color)};
        hotCuesSet++;
    }
    snapshot.hot_cues = std::move(slots);
    if (earliestMemoryCueMs) {
        snapshot.main_cue = *earliestMemoryCueMs / 1000.0 * DefaultSampleRate;
    }
    return hotCuesSet + (earliestMemoryCueMs ? 1 : 0);
}

// One track's place in one playlist: where it sits, and the row that was
// created for it. Held until every track exists, because a playlist can
// only be filled with tracks the database already has.
struct PlaylistMember
{
    int position = -1;
    djinterop::track track;
};

using PlaylistMembers = std::map<std::string, std::vector<PlaylistMember>>;

// Rebuilds the source's playlists, folders and all.
//
// A membership's name is a full path ("Techno/Peak Time"), so each
// segment but the last is a folder. Engine models both with the same
// Playlist table, which is why a folder here is simply a playlist with
// children -- created once and reused, so two playlists under the same
// folder share it rather than producing two folders of the same name.
//
// Order matters to a DJ: a playlist is not a set. Tracks go in by the
// position the source recorded, and anything the reader could not place
// (position -1) keeps its encounter order at the end rather than being
// dropped or silently sorted somewhere arbitrary.
//
// Returns how many playlists were created, folders included.
int createPlaylists(djinterop::database &db, const PlaylistMembers &members,
                     application::ProgressReporter &reporter, const application::CancellationToken &cancel)
{
    if (members.empty()) {
        return 0;
    }
    reporter.start("Creating playlists", members.size());

    std::map<std::string, djinterop::playlist> byPath;
    int created = 0;
    size_t done = 0;

    // Creates the playlist at `path`, and every folder above it, once.
    const std::function<std::optional<djinterop::playlist>(const std::string &)> ensure =
        [&](const std::string &path) -> std::optional<djinterop::playlist> {
        if (auto existing = byPath.find(path); existing != byPath.end()) {
            return existing->second;
        }
        const size_t cut = path.rfind('/');
        const std::string leaf = cut == std::string::npos ? path : path.substr(cut + 1);
        if (leaf.empty()) {
            return std::nullopt;
        }
        try {
            djinterop::playlist playlist = cut == std::string::npos
                                                ? db.create_root_playlist(leaf)
                                                : [&]() -> djinterop::playlist {
                auto parent = ensure(path.substr(0, cut));
                if (!parent) {
                    return db.create_root_playlist(leaf);
                }
                return parent->create_sub_playlist(leaf);
            }();
            ++created;
            byPath.insert({path, playlist});
            return playlist;
        } catch (const std::exception &) {
            // A name Engine will not take (a duplicate, or characters it
            // rejects) costs that one playlist, not the whole library:
            // the tracks themselves are already in and playable.
            return std::nullopt;
        }
    };

    for (const auto &[path, entries] : members) {
        if (cancel.cancelled()) {
            break;
        }
        auto playlist = ensure(path);
        if (playlist) {
            std::vector<PlaylistMember> ordered = entries;
            std::stable_sort(ordered.begin(), ordered.end(), [](const PlaylistMember &a, const PlaylistMember &b) {
                const bool aPlaced = a.position >= 0;
                const bool bPlaced = b.position >= 0;
                if (aPlaced != bPlaced) {
                    return aPlaced;  // unplaced tracks go last, in encounter order
                }
                return aPlaced ? a.position < b.position : false;
            });
            for (const PlaylistMember &member : ordered) {
                try {
                    playlist->add_track_back(member.track);
                } catch (const std::exception &) {
                    // Same reasoning: one missing entry, not a failed build.
                }
            }
        }
        reporter.tick(++done);
    }
    reporter.finish();
    return created;
}

// Where libdjinterop put the database: the 1.x generation keeps m.db at
// the library root, 2.x and 3.x under Database2. Empty if neither is
// there.
std::filesystem::path engineDatabaseFile(const std::filesystem::path &databaseDirectory)
{
    std::error_code ec;
    std::filesystem::path candidate = databaseDirectory / "Database2" / "m.db";
    if (std::filesystem::exists(candidate, ec)) {
        return candidate;
    }
    candidate = databaseDirectory / "m.db";
    if (std::filesystem::exists(candidate, ec)) {
        return candidate;
    }
    return {};
}

// base64url, unpadded -- how Engine spells a hash when it becomes a file
// name under Artwork/.
std::string base64Url(std::span<const std::uint8_t> bytes)
{
    static constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const std::uint32_t triple = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        out += Alphabet[(triple >> 18) & 0x3F];
        out += Alphabet[(triple >> 12) & 0x3F];
        out += Alphabet[(triple >> 6) & 0x3F];
        out += Alphabet[triple & 0x3F];
    }
    if (i + 1 == bytes.size()) {
        const std::uint32_t triple = bytes[i] << 16;
        out += Alphabet[(triple >> 18) & 0x3F];
        out += Alphabet[(triple >> 12) & 0x3F];
    } else if (i + 2 == bytes.size()) {
        const std::uint32_t triple = (bytes[i] << 16) | (bytes[i + 1] << 8);
        out += Alphabet[(triple >> 18) & 0x3F];
        out += Alphabet[(triple >> 12) & 0x3F];
        out += Alphabet[(triple >> 6) & 0x3F];
    }
    return out;
}

// Gives the new library its cover art, the way Engine itself stores it.
//
// Engine does not embed artwork in the database. Its AlbumArt row carries
// a 20-byte hash as a blob, and the image lives at
// "Artwork/<that hash, base64url>.jpg" inside the library: on a
// Denon-written stick every one of the 87 files under Artwork/ is named
// by exactly that encoding of its row's hash. (The same table also holds
// rows whose "hash" is really an "image://fileart//<absolute path>"
// string left over from an import; not one track on that stick points at
// one, so they are bookkeeping, and an absolute path with a mount point
// in it would not survive being plugged into a different machine
// anyway.)
//
// libdjinterop cannot do any of this -- its album_art API is unfinished
// and track_snapshot has no artwork field at all -- so it is plain SQL
// plus a file copy, against the scratch build.
//
// The source is whatever the rekordbox side already resolved for each
// track, which is a file sitting on the same stick. One image usually
// covers a whole album, so images are written once per distinct hash and
// the rows share it.
//
// Returns how many tracks ended up with art, or -1 if the database could
// not be opened at all. An individual unreadable image is skipped: art is
// the least important thing here, and losing one cover is not worth
// failing a library whose tracks are otherwise ready to play.
// Which spelling of Track's album-art column this database uses.
//
// V1 libraries declare [idAlbumArt]; 2.x and 3.x renamed it to
// albumArtId. Writing the 2.x name at a V1 library threw into the
// per-track catch below, so the image was copied and the AlbumArt row
// inserted while nothing was ever pointed at it -- covers under
// Artwork/ that no track referenced, orphan rows, and a count of zero
// that read as "this library had no art".
//
// Asked of the database rather than derived from the generation the
// caller asked for, so a library made by anything else still gets
// pointed at correctly. Empty when Track has neither, which means: do
// not write.
std::string albumArtColumnName(sqlite3 *db)
{
    try {
        local::Statement columns(db, "PRAGMA table_info(Track);", "Engine library creator");
        while (columns.step()) {
            const std::string name = columns.columnText(1);
            if (name == "albumArtId" || name == "idAlbumArt") {
                return name;
            }
        }
    } catch (const std::exception &) {
        return {};
    }
    return {};
}

int copyArtworkInto(const std::filesystem::path &databaseDirectory,
                     const std::map<std::string, std::string> &artworkByTrackPath)
{
    if (artworkByTrackPath.empty()) {
        return 0;
    }
    const std::filesystem::path databaseFile = engineDatabaseFile(databaseDirectory);
    if (databaseFile.empty()) {
        return -1;
    }
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(databaseFile.string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    const std::filesystem::path artworkDir = databaseDirectory / "Artwork";
    std::error_code ec;
    std::filesystem::create_directories(artworkDir, ec);

    // Every -1 below leaves the database with no AlbumArt rows, and the
    // images this function has already written are then files nothing
    // points at. The caller copies the whole scratch tree to the stick
    // regardless, so without this they travel there as orphans while the
    // count says 0 covers: space taken on the stick that no player and
    // no cleanup in this app would ever account for. Emptied rather than
    // removed, so the library keeps the directory it is supposed to have.
    const auto giveUp = [&](sqlite3 *handle) {
        sqlite3_close(handle);
        std::error_code cleanupEc;
        std::filesystem::remove_all(artworkDir, cleanupEc);
        std::filesystem::create_directories(artworkDir, cleanupEc);
        return -1;
    };

    const std::string albumArtColumn = albumArtColumnName(db);
    if (albumArtColumn.empty()) {
        return giveUp(db);
    }
    const std::string pointStatement = "UPDATE Track SET " + albumArtColumn + " = ? WHERE path = ?;";

    std::map<std::string, std::int64_t> albumArtIdBySource;  // source image -> row it got
    int given = 0;
    // Checked, both ends. A BEGIN that does not take means every write
    // below autocommits, and a COMMIT that fails means none of them
    // landed -- either way the count returned would describe a database
    // that does not exist.
    if (sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return giveUp(db);
    }
    for (const auto &[trackPath, source] : artworkByTrackPath) {
        std::int64_t albumArtId = 0;
        if (auto seen = albumArtIdBySource.find(source); seen != albumArtIdBySource.end()) {
            albumArtId = seen->second;
        } else {
            std::ifstream image(source, std::ios::binary);
            if (!image) {
                continue;
            }
            const std::string bytes((std::istreambuf_iterator<char>(image)), std::istreambuf_iterator<char>());
            if (bytes.empty()) {
                continue;
            }
            // 20 bytes, because that is the width Engine's own rows use.
            const auto full = hashing::Sha256::of(std::as_bytes(std::span(bytes)));
            const std::span<const std::uint8_t> shortened(full.data(), 20);
            const std::string name = base64Url(shortened);
            // From the bytes, never from the source's name. Engine looks
            // for "<hash>.jpg", ".jpeg" or ".png" exactly, so a source
            // called a5_m.PNG or cover.webp was written under a name no
            // player looks for while the track was still counted as
            // given. Empty means neither JPEG nor PNG: skip it rather
            // than name a file we cannot promise a player reads. Same
            // rule the repair path uses, and now literally the same
            // function.
            const std::string extension = extensionForImage(bytes);
            if (extension.empty()) {
                continue;
            }

            std::ofstream copy(artworkDir / (name + extension), std::ios::binary);
            if (!copy) {
                continue;
            }
            copy.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            copy.close();

            try {
                local::Statement insert(db, "INSERT INTO AlbumArt (hash, albumArt) VALUES (?, NULL);",
                                         "Engine library creator");
                // A blob, which is how Engine's own rows store it -- not
                // text, even though the column is declared TEXT.
                insert.bindBlob(1, std::string(reinterpret_cast<const char *>(shortened.data()), shortened.size()));
                insert.run();
            } catch (const std::exception &) {
                continue;
            }
            albumArtId = sqlite3_last_insert_rowid(db);
            albumArtIdBySource.insert({source, albumArtId});
        }

        // The path is what identifies the row: it is unique by schema
        // constraint, and it is what was just written for this track.
        try {
            local::Statement point(db, pointStatement.c_str(), "Engine library creator");
            point.bindInt64(1, albumArtId);
            point.bind(2, trackPath);
            point.run();
            if (sqlite3_changes(db) > 0) {
                ++given;
            }
        } catch (const std::exception &) {
            continue;
        }
    }
    if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        // The scratch build lives in /tmp, which is tmpfs here, so "the
        // volume filled" is not hypothetical. Reporting `given` after a
        // failed commit would claim art that is in no database, with the
        // image files sitting on the stick regardless.
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return giveUp(db);
    }
    sqlite3_close(db);
    return given;
}

// Hands the waveform to the player, by leaving the tracks looking
// exactly like the ones Engine's own rekordbox import leaves behind.
//
// This project has no rekordbox->Engine waveform conversion, so every
// track it creates reaches the stick without one. libdjinterop writes a
// PerformanceData row for each track anyway and marks it analysed
// (v3/track_impl.cpp hard-codes is_analyzed = true), so the player
// believes the analysis is already done, never runs its own, and shows
// an empty waveform for ever. That is the whole reason a created
// library plays fine but draws nothing.
//
// A Denon-written stick shows what the other state looks like: of its
// 1566 tracks, 1219 are imported-but-not-yet-analysed, and every one of
// them has isAnalyzed = 0 with NULL in trackData, overviewWaveFormData
// and beatData, while quickCues and loops still carry the cues the
// import brought over. The remaining 347 have been analysed on the
// device and have all three columns filled in. So the player treats
// that combination as work still to do, and it keeps the cues.
//
// The beatgrid goes with them, because it lives in beatData. The player
// recomputes it while analysing, as it does for its own imports, and
// ours was only ever a two-point approximation from BPM and duration
// anyway. Everything the player cannot derive stays where it is: title,
// artist, bpm, key, rating, length and the cues.
//
// When this project can convert a real waveform, this pass has to
// become conditional on not having one, rather than unconditional as it
// is here.
//
// Returns the number of tracks handed over, or -1 if the database could
// not be opened. A failure costs the waveform display and nothing else,
// so it is reported as a count rather than as an error, the same way
// cover art is.
int markTracksForDeviceAnalysis(const std::filesystem::path &databaseDirectory)
{
    const std::filesystem::path databaseFile = engineDatabaseFile(databaseDirectory);
    if (databaseFile.empty()) {
        return -1;
    }
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(databaseFile.string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    const auto run = [db](const char *sql) {
        return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    };

    // 2.x and 3.x only. A 1.x library keeps its performance data -- the
    // flag included -- in a second database file beside this one, and no
    // 1.x hardware has been available to check what a track waiting for
    // analysis looks like there, so those libraries are left exactly as
    // libdjinterop wrote them.
    //
    // Prepared by hand rather than through local::Statement: that one
    // throws when a statement will not prepare, and an exception leaving
    // here would close nothing and turn a missing waveform into a failed
    // creation with nothing copied to the stick -- the opposite of what
    // this function promises.
    bool v2OrLater = false;
    {
        sqlite3_stmt *shape = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT count(*) FROM pragma_table_info('Track') WHERE name = 'isAnalyzed';",
                                -1, &shape, nullptr)
                == SQLITE_OK
            && sqlite3_step(shape) == SQLITE_ROW) {
            v2OrLater = sqlite3_column_int(shape, 0) == 1;
        }
        sqlite3_finalize(shape);
    }
    if (!v2OrLater) {
        sqlite3_close(db);
        return 0;
    }

    // The blobs first: a track that is already marked unanalysed while
    // still carrying analysis data would be the one state the player has
    // never been observed in.
    int marked = -1;
    if (run("BEGIN;") &&
        run("UPDATE PerformanceData SET trackData = NULL, overviewWaveFormData = NULL, "
            "beatData = NULL;") &&
        run("UPDATE Track SET isAnalyzed = 0;")) {
        marked = sqlite3_changes(db);
        if (!run("COMMIT;")) {
            marked = -1;
        }
    }
    if (marked < 0) {
        run("ROLLBACK;");
    }
    sqlite3_close(db);
    return marked;
}

// Puts the Information row back at id 1, where Engine keeps it.
//
// The Information table is the first thing anything reading an Engine
// library looks at -- it is where the schema version lives, and
// libdjinterop's own detect_schema() reads it. Every library Engine
// itself writes holds exactly one row there, at id 1: confirmed against
// a Denon-written stick and against the database a Prime 4 creates for
// itself.
//
// libdjinterop's 3.0.2 creator does not. schema_3_0_2::create() seeds
// the AUTOINCREMENT counter for the table ("INSERT INTO sqlite_sequence
// VALUES('Information',1)") *before* inserting the row, so the row lands
// at id 2 and the counter reads 2. No other schema version in that file
// does this, and 3.0.2 is exactly the schema current Denon hardware
// uses. A Prime 4 rejected a library created this way as corrupt, and
// stopped rejecting it once the row was renumbered -- which is why this
// is corrected here rather than left to the caller to notice.
//
// Done with plain SQL because libdjinterop exposes no way to reach the
// row: the id is not part of any public API. Runs against the scratch
// copy, before a single byte is written to the destination.
//
// Returns an error message, or an empty string when the row is where it
// belongs (including when it already was, so a fixed or newly vendored
// libdjinterop makes this a no-op rather than a second bug).
std::string putInformationRowAtIdOne(const std::filesystem::path &databaseDirectory)
{
    const std::filesystem::path databaseFile = engineDatabaseFile(databaseDirectory);
    if (databaseFile.empty()) {
        return "the new Engine database was not written where it was expected.";
    }
    const std::string dbPath = databaseFile.string();
    sqlite3 *db = nullptr;
    // READWRITE without CREATE: a wrong path must fail here, not leave a
    // stray empty database behind for the copy to carry onto the stick.
    if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        const std::string message = db ? sqlite3_errmsg(db) : "could not open the new database";
        sqlite3_close(db);
        return "could not open the new Engine database to check its Information row: " + message;
    }
    const auto run = [db](const char *sql) -> std::string {
        char *error = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &error) == SQLITE_OK) {
            return {};
        }
        const std::string message = error ? error : "unknown error";
        sqlite3_free(error);
        return message;
    };
    // One statement each, so a database that already has it right is
    // left completely untouched.
    std::string failure = run("UPDATE Information SET id = 1 WHERE id <> 1 "
                              "AND (SELECT count(*) FROM Information) = 1;");
    if (failure.empty()) {
        failure = run("UPDATE sqlite_sequence SET seq = 1 WHERE name = 'Information' AND seq <> 1;");
    }

    // Verify rather than assume: this exists precisely because a library
    // that looks fine to us can still be refused by the hardware, so the
    // one thing it fixes is checked before the copy proceeds.
    std::string verified;
    if (failure.empty()) {
        local::Statement check(db, "SELECT count(*), coalesce(min(id), 0) FROM Information;",
                                "Engine library creator");
        if (check.step()) {
            const int rows = check.columnInt(0);
            const int firstId = check.columnInt(1);
            if (rows != 1 || firstId != 1) {
                verified = "the new Engine database has " + std::to_string(rows) +
                            " Information row(s), the first at id " + std::to_string(firstId) +
                            " -- Engine expects exactly one, at id 1.";
            }
        } else {
            verified = "could not read back the new Engine database's Information row.";
        }
    }
    sqlite3_close(db);
    if (!failure.empty()) {
        return "could not correct the new Engine database's Information row: " + failure;
    }
    return verified;
}

}  // namespace

using infrastructure::ScratchDirGuard;

EngineLibraryCreationResult EngineLibraryCreator::create(const std::string &directory,
                                                           const std::vector<domain::Track> &tracks,
                                                           EngineSchemaGeneration schemaGeneration,
                                                           application::ProgressReporter &reporter,
                                                           const application::CancellationToken &cancel)
{
    EngineLibraryCreationResult result;
    result.tracksTotal = static_cast<int>(tracks.size());

    if (djinterop::engine::database_exists(directory)) {
        result.errorMessage = "An Engine Library already exists at " + directory + " -- refusing to overwrite it.";
        return result;
    }

    // Every track insert is its own implicit SQLite transaction (libdjinterop
    // exposes no public transaction control), which means its own fsync.
    // On fast local storage that's cheap enough to hide; on the slow
    // removable media this library is actually destined for, thousands of
    // individual fsyncs is what makes a large library take a very long
    // time. Rather than patching the vendored library to add transaction
    // batching, the whole database is instead built in a scratch directory
    // on local storage (temp_directory_path() -- /tmp is tmpfs on most
    // Linux setups, and even when it isn't, it's still far faster than a
    // USB stick or SD card), then copied onto the real target in one pass
    // at the end -- a handful of large sequential writes instead of one
    // fsync per track. As a side effect, this also means a crash or a
    // yanked cable during the (now fast) build phase leaves the real stick
    // completely untouched, instead of the half-written "Engine Library"
    // folder this feature used to leave behind before this change.
    fs::path scratchDir = fs::temp_directory_path() /
                           ("seabass-engine-build-" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code cleanupBeforeStart;
    fs::remove_all(scratchDir, cleanupBeforeStart);
    ScratchDirGuard scratchGuard{scratchDir};

    // Each created track's path in the new library, against the image the
    // rekordbox side already resolved for it. Outlives the block below,
    // because the art is written once that database connection is closed.
    std::map<std::string, std::string> artworkByTrackPath;

    try {
        {
            auto db = djinterop::engine::create_database(scratchDir.string(), schemaFor(schemaGeneration));

            reporter.start("Creating Engine Library", tracks.size());
            size_t processed = 0;
            // Filled as tracks are created, spent once they all exist.
            PlaylistMembers playlistMembers;

            for (const auto &track : tracks) {
                if (cancel.cancelled()) {
                    // The scratch copy dies with scratchGuard; the stick
                    // has not been touched yet.
                    result.cancelled = true;
                    reporter.finish();
                    return result;
                }
                // Streaming tracks have no local file by design (see
                // Track::streamingSource's own doc comment) and rows with no
                // resolved file path can't be referenced from a fresh
                // library at all -- both are skipped, not fabricated.
                if (!track.streamingSource.empty() || track.filePath.empty()) {
                    result.tracksSkipped++;
                    reporter.tick(++processed);
                    continue;
                }

                djinterop::track_snapshot snapshot;
                snapshot.title = track.title.empty() ? std::nullopt : std::optional(track.title);
                snapshot.artist = track.artist.empty() ? std::nullopt : std::optional(track.artist);
                if (track.bpm > 0.0) {
                    snapshot.bpm = track.bpm;
                }
                if (auto key = parseRekordboxKey(track.key)) {
                    snapshot.key = *key;
                }
                if (track.durationSeconds > 0.0) {
                    snapshot.duration =
                        std::chrono::milliseconds(static_cast<int64_t>(track.durationSeconds * 1000.0));
                }
                if (track.bitrate > 0) {
                    snapshot.bitrate = track.bitrate;
                }
                if (track.rating.has_value()) {
                    // Track::rating is 0-5 stars; Engine's own scale is 0-100.
                    snapshot.rating = *track.rating * 20;
                }
                if (!track.comment.empty()) {
                    snapshot.comment = track.comment;
                }
                if (track.fileSizeBytes > 0) {
                    snapshot.file_bytes = track.fileSizeBytes;
                }

                std::error_code relError;
                fs::path relative = fs::relative(track.filePath, directory, relError);
                snapshot.relative_path = relError ? track.filePath : relative.generic_string();

                // Simple, approximate two-point beatgrid: assumes the track
                // starts exactly on a downbeat at sample 0, then a second
                // marker far enough out to cover the whole track at a
                // constant BPM. Not a substitute for a real per-beat grid --
                // see this class's own doc comment for why that's out of
                // scope for now -- but gives Engine something to quantize/
                // sync against rather than nothing at all.
                if (track.bpm > 0.0 && track.durationSeconds > 0.0) {
                    double secondsPerBeat = 60.0 / track.bpm;
                    double totalBeats = track.durationSeconds / secondsPerBeat;
                    int64_t sampleCount = static_cast<int64_t>(track.durationSeconds * DefaultSampleRate);
                    std::vector<djinterop::beatgrid_marker> grid = {
                        {0, 0.0},
                        {static_cast<int>(totalBeats), totalBeats * secondsPerBeat * DefaultSampleRate},
                    };
                    snapshot.beatgrid = djinterop::engine::normalize_beatgrid(grid, sampleCount);
                    snapshot.sample_rate = DefaultSampleRate;
                    snapshot.sample_count = static_cast<unsigned long long>(sampleCount);
                }

                result.cuesCopied += applyCuesToSnapshot(snapshot, track.cues);

                djinterop::track created = db.create_track(snapshot);
                result.tracksCreated++;
                if (!track.artworkPath.empty()) {
                    std::error_code artEc;
                    if (fs::exists(track.artworkPath, artEc)) {
                        artworkByTrackPath[snapshot.relative_path.value_or("")] = track.artworkPath;
                    }
                }
                for (const domain::PlaylistMembership &membership : track.playlists) {
                    if (!membership.name.empty()) {
                        playlistMembers[membership.name].push_back({membership.position, created});
                    }
                }
                reporter.tick(++processed);
            }
            reporter.finish();

            result.playlistsCreated = createPlaylists(db, playlistMembers, reporter, cancel);
            // db goes out of scope here, closing its SQLite connection (and
            // with it, any pending journal) before the raw files underneath
            // are copied below -- copying while the connection is still open
            // would risk copying an inconsistent file.
        }

        // Still on the scratch copy, with the connection closed: the one
        // correction that decides whether real hardware will accept this
        // library at all. A failure here stops before the copy, so the
        // destination keeps whatever it had (nothing) rather than
        // receiving a library the player would refuse.
        if (std::string informationRow = putInformationRowAtIdOne(scratchDir); !informationRow.empty()) {
            result.errorMessage = informationRow;
            return result;
        }

        // Cover art, also on the scratch copy. Unlike the row above this
        // is not a condition of the library working: a failure here costs
        // the covers, so it is reported as a count rather than an error.
        const int artworkGiven = copyArtworkInto(scratchDir, artworkByTrackPath);
        result.artworkCopied = artworkGiven < 0 ? 0 : artworkGiven;

        // Also on the scratch copy, and also not a condition of the
        // library working: without this the player believes every track
        // has already been analysed and draws an empty waveform.
        const int marked = markTracksForDeviceAnalysis(scratchDir);
        result.tracksLeftForDeviceAnalysis = marked < 0 ? 0 : marked;

        // One pass of large sequential writes onto the real target, instead
        // of the many small fsync'd writes the per-track loop above would
        // otherwise have done directly against it.
        reporter.start("Copying to stick", 1);
        fs::copy(scratchDir, directory, fs::copy_options::recursive);
        reporter.tick(1);
        reporter.finish();
    } catch (const std::exception &e) {
        result.errorMessage = e.what();
    }

    return result;
}

}  // namespace seabass::infrastructure::engine
