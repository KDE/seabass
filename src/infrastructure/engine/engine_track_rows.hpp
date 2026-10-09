// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::infrastructure::engine
{

// Track rows in an Engine 2.x/3.x m.db: created in a library a player
// already owns, and removed with plain SQL where libdjinterop would leave
// rows behind.

// Removes the tracks with these ids from the m.db at `databaseFile` (a
// write root's database: the stick's own or a save's scratch copy), with
// every row that names them, in one transaction:
//
//   - their PlaylistEntity rows, from every playlist. Engine's own trigger
//     (trigger_before_delete_PlaylistEntity) relinks each playlist's
//     nextEntityId chain around a deleted entry, so the other members keep
//     their order;
//   - their PreparelistEntity rows;
//   - their PerformanceData row (a table from 2.20 on, a view over Track
//     before, which goes with the row);
//   - ChangeLog, where it is a table (2.18 to 2.20; a view after): its
//     trackId is set to NULL, which is what its declared foreign key
//     (ON DELETE SET NULL) asks for. The log entry itself stays;
//   - then the Track row.
//
// libdjinterop's remove_track deletes the Track row alone and relies on
// ON DELETE CASCADE, which SQLite only honours with PRAGMA foreign_keys
// on, which nothing turns on; and PlaylistEntity declares no key to Track
// at all. So its removal leaves the track's performance data and playlist
// entries behind.
//
// Deliberately left: the AlbumArt row the track pointed at (rows are
// shared between tracks, and one with no track left is harmless), and the
// audio file (it belongs to rekordbox, or to whoever put it on the stick;
// Delete Tracks queues it separately).
//
// Unlike removeEngineTracks this removes tracks that are in playlists:
// that is what it is for. A caller with a rule about listed tracks applies
// it before calling.
//
// Returns how many Track rows were removed, which is trackIds.size() on
// success. Returns -1 with *error saying why, and the database as it was
// (the transaction is rolled back), when: the database cannot be opened
// or written; an id has no Track row (an unknown id is an error, not a
// skip, so the caller's count cannot quietly differ from what went); an
// id is named twice; or, after the deletes, some table with a trackId
// column still names a removed track (a schema this does not know). On
// success *error is cleared. `error` is required: a removal that failed
// without saying why would leave the caller with a count and no reason.
[[nodiscard]] int removeEngineTrackRows(const std::string &databaseFile, const std::vector<std::int64_t> &trackIds,
                                        std::string *error);

// How many rows in the m.db at `databaseFile` name track `trackId`: its
// Track row plus every row of every table with a trackId column (views
// are not counted; they hold nothing of their own). Read-only. Returns -1
// with *error saying why when the database cannot be read.
[[nodiscard]] int engineRowsNamingTrack(const std::string &databaseFile, std::int64_t trackId, std::string *error);

// The path an Engine row keeps for `trackFile`, relative to the Engine
// Library folder `realEngineLibraryPath` on the stick (the real one, not a
// save's scratch copy: the row is read on the stick), in the generic '/'
// spelling, UTF-8.
//
// Lifted from EngineLibraryCreator with both of its cases:
//   - fs::relative does not report "no relation" as an error. It is
//     lexically_relative underneath and returns an EMPTY path when the
//     two share no root (different Windows drives), with the error code
//     clear. Then the track's own path comes back, absolute;
//   - always the generic spelling: libdjinterop's get_filename only splits
//     on '/', so a backslash path reads as one file name with no folder
//     and, without a dot in it, create_track refuses it for having no
//     extension.
[[nodiscard]] std::string engineRelativePath(const std::string &trackFile, const std::string &realEngineLibraryPath);

// One track to add to an existing Engine library.
struct NewEngineTrack
{
    // What the row is made from: title, artist, bpm, key, durationSeconds,
    // bitrate, rating (stars), comment, fileSizeBytes, filePath (absolute,
    // on the stick as it is mounted now), cues (the rekordbox side's, hot
    // and memory; translated for Engine here) and artworkPath (the image
    // the rekordbox side resolved, or empty). Playlists are not this
    // function's business.
    domain::Track source;
    // The audio file's own sample rate, in Hz, read from the file
    // (application::TrackMetadataProbe). Engine keeps every cue as a
    // sample offset, so this decides where each cue lands. Never a
    // default: 0 or less is refused.
    double sampleRateHz = 0.0;
    // Not in Track: the Engine Library folder on the stick, which the
    // row's path is relative to (engineRelativePath) and where a cover
    // file goes (Artwork/ is never redirected into a save's scratch copy).
    std::string realEngineLibraryPath;
};

// The cover half of createEngineTrack, in and out. Required: a row that
// came without the cover it was given must be visible to the caller.
struct EngineTrackCover
{
    // In: called with each file about to be created under Artwork/, before
    // it exists (SaveContext::protectForThisChange). May throw, which
    // leaves the cover unwritten and the row in place.
    std::function<void(const std::string &)> beforeWrite;
    // Out: whether the track now points at its cover, the files written,
    // and why not when source.artworkPath was set and it does not.
    bool written = false;
    std::vector<std::string> filesWritten;
    std::string problem;
};

// Adds one Track row to the Engine 2.x/3.x library at `writeRoot` (the
// Engine Library layout: the stick's own, or a save's scratch copy of it,
// FormatWriteSession::writeRoot) and returns its id.
//
// In order:
//   1. refuses, writing nothing: no m.db under writeRoot (an unopenable
//      root, or Engine 1.x, whose m.db sits at the root and keeps its
//      analysis elsewhere), a streaming track, a file that is not there,
//      a sample rate <= 0, a path that cannot be made relative to the
//      library (a file on another Windows drive), or a path the library
//      already has a row for, compared without ASCII case (the sticks are
//      FAT, exFAT or HFS+; one file spelled twice is still one file);
//   2. creates the row through libdjinterop's create_track with the
//      creator's field mapping: title, artist, bpm, key (parsed as
//      rekordbox writes it), duration, bitrate, rating times 20, comment,
//      file bytes, the relative path. No beat grid and no waveform: the
//      player makes both when it analyses the track;
//   3. markForDeviceAnalysis on this row alone;
//   4. the cues, translated with domain::translateCuesForEngine (no
//      existing cues: a new row has none), written through
//      LibdjinteropEngineCueWriter at `sampleRateHz`;
//   5. the cover, when source.artworkPath is set, through repairArtwork:
//      the library's own storage (a file under Artwork/ or the database),
//      `cover.beforeWrite` told first. A cover that cannot be written does
//      not undo the row; cover.problem says why;
//   6. the Information table is read again and must be the one row it
//      was before, byte for byte (its id is not assumed: Engine's own are
//      at 1, a player's has been seen at 2).
// A failure in 2, 3, 4 or 6 removes the row again (removeEngineTrackRows)
// and returns -1 with *error saying why.
//
// What the row says that the creator's do too, as libdjinterop writes
// them: pdbImportKey 0 (no player import produced this row, and faking
// one would tie it to an export.pdb row it never came from), dateAdded
// the time of this call, dateCreated 0, isMetadataImported 1,
// originDatabaseUuid this library's uuid, originTrackId the new id (by
// Engine's own trigger). isAnalyzed 0 with NULL trackData,
// overviewWaveFormData and beatData, which is how a player-imported row
// looks before the player has loaded it.
//
// Until the player analyses the row, trackData is NULL, so nothing in
// the row records the sample rate: LibdjinteropEngineReader then reads
// the cues at its 44.1 kHz guess, 9% late for a 48 kHz file. The offsets
// written are the file's own; it is the reading that guesses.
//
// `error` and `cover` are required (std::invalid_argument). On success
// *error is cleared.
[[nodiscard]] std::int64_t createEngineTrack(const std::string &writeRoot, const NewEngineTrack &track,
                                             EngineTrackCover *cover, std::string *error);

// Marks these Track rows of the m.db at `databaseFile` as waiting for the
// player's analysis, the state Engine's own rekordbox import leaves a row
// in: isAnalyzed = 0, and NULL in PerformanceData's trackData,
// overviewWaveFormData and beatData (the blobs first, as the creator's
// statement does). Quick cues and loops are kept. Each row by its id,
// never the library: the other rows are the player's analysis.
//
// The same statements the creator runs over a fresh library
// (markTracksForDeviceAnalysis), per id. Works on 2.x and 3.x, where
// PerformanceData is a table (2.20 on) or a view over Track (before).
//
// Returns how many rows were marked, ids.size() on success, in one
// transaction. Returns -1 with *error, and nothing changed, for: a
// database that cannot be opened; one without Track.isAnalyzed (Engine
// 1.x keeps the flag in another file; refused rather than skipped); an
// id with no Track row; an id named twice. `error` is required.
[[nodiscard]] int markForDeviceAnalysis(const std::string &databaseFile, const std::vector<std::int64_t> &ids,
                                        std::string *error);

// The Information table of the m.db at `databaseFile`: every row, every
// column as SQLite holds it (text, numbers as text, blobs as hex), rows
// in id order. Read-only. nullopt with *error when it cannot be read,
// SQLite's own words included ("no such table: Information" is the whole
// diagnosis).
struct EngineInformationRows
{
    std::vector<std::int64_t> ids;
    std::vector<std::vector<std::optional<std::string>>> values;
};
[[nodiscard]] std::optional<EngineInformationRows> readEngineInformation(const std::string &databaseFile,
                                                                         std::string *error);

}  // namespace seabass::infrastructure::engine
