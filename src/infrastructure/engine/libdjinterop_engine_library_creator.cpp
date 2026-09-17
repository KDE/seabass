// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/engine/libdjinterop_engine_library_creator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <djinterop/djinterop.hpp>
#include <djinterop/engine/engine.hpp>
#include <sqlite3.h>

#include "infrastructure/engine/rekordbox_key_parser.hpp"
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
    // Two layouts: the 1.x generation keeps m.db at the library root, 2.x
    // and 3.x put it under Database2. Both carry the Information table, so
    // look for the one that is actually there rather than assuming.
    std::filesystem::path databaseFile = databaseDirectory / "Database2" / "m.db";
    std::error_code ec;
    if (!std::filesystem::exists(databaseFile, ec)) {
        databaseFile = databaseDirectory / "m.db";
    }
    if (!std::filesystem::exists(databaseFile, ec)) {
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
