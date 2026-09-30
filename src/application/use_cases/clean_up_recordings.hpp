// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/track_duration_probe.hpp"

namespace seabass::application
{

// Set recordings the DJ hardware leaves on a stick, and deleting them.
//
// Engine OS players (Prime 4, Go+, the SC series) record a set into a
// top-level `Sessions/` folder as WAV: `Session-0001.wav` by default, or
// whatever the DJ renamed it to. Pioneer and AlphaTheta gear record the
// master output as `REC***.WAV` into a top-level folder of their own. A
// real stick carries gigabytes of these (WHALESHARK: six files, 6.2 GB),
// and nothing in any of the three libraries refers to them: they are the
// DJ's own files, not a library's, and usually long since copied off and
// reworked elsewhere. So this deletes them and copies nothing: the page
// says so before anything goes.
//
// So nothing here goes near a library. Only audio files sitting DIRECTLY
// inside one of the folders below are ever listed or touched; anything
// else in such a folder is reported as left alone, and the folder itself
// stays (the hardware recreates it anyway). `Contents/`, `PIONEER/`,
// `Engine Library/` and `Seabass/` cannot be reached: the listing only
// opens folders whose name is one of these, and deleteRecordings() checks
// every path it is handed against the same rule again before it touches
// it, rather than trusting the listing it came from.

enum class RecordingSource
{
    EngineOs,    // Sessions/
    Pioneer,     // PIONEER REC/, PIONEER DJ REC/
    AlphaTheta,  // ALPHATHETA REC/
};

// The folder names, compared case-insensitively (FAT and exFAT do not
// care about case, and a stick read on Linux can come back in either).
//
// Spellings and what backs each one, because two of them are not settled:
// - "Sessions": Engine OS, seen on real sticks (WHALESHARK, A4-128GB).
// - "PIONEER REC": the OMNIS-DUO manual, a forum post about the XDJ-RX2,
//   and an empty folder of that name on WHALESHARK dated 2017-01-01, the
//   date the XDJ-RX2 (no clock) stamps its export.pdb with.
// - "PIONEER DJ REC": one summary of the XDJ-RX2 manual. Unconfirmed.
// - "ALPHATHETA REC": an empty folder of that name on WHALESHARK dated
//   2026-07-11, which only the OMNIS-DUO can have made. No file seen in it.
struct RecordingFolderSpelling
{
    const char *name;
    RecordingSource source;
};
const std::vector<RecordingFolderSpelling> &recordingFolderSpellings();

// "engine", "pioneer", "alphatheta": the key the GUI turns into words.
const char *recordingSourceKey(RecordingSource source);

// Whether a file name is one this feature treats as a recording: an audio
// extension (wav, aif, aiff, mp3, flac, m4a; any case), and not hidden --
// macOS leaves a 4 KB "._Session-0001.wav" beside every file it touched,
// and that is not a recording however it ends.
bool isRecordingFileName(const std::string &fileNameUtf8);

struct Recording
{
    std::string path;        // absolute, UTF-8
    std::string fileName;    // UTF-8
    std::string folderName;  // as spelled on the stick
    RecordingSource source = RecordingSource::EngineOs;
    std::uint64_t sizeBytes = 0;
    std::int64_t modifiedUnix = 0;
    // From the probe; nullopt when it could not tell (or none was given
    // a chance: the hub's summary does not ask).
    std::optional<double> durationSeconds;
};

// Something in a recording folder that is not a recording.
struct RecordingFolderLeftover
{
    std::string path;
    std::string fileName;
    std::string folderName;
    std::string reason;  // "not an audio file", "a folder", "hidden file", ...
};

struct RecordingListing
{
    std::vector<Recording> recordings;  // sorted by folder, then name
    std::vector<RecordingFolderLeftover> leftAlone;
    // A recording folder that is there but could not be read to the end.
    // Said, never folded into "no recordings".
    std::vector<std::string> unreadableFolders;
    std::uint64_t totalBytes = 0;
};

// Lists the recordings on the stick at `stickRoot`. Reads directory
// entries and stats, plus whatever `durations` reads to answer (TagLib
// reads a WAV's header only). Pass a NullTrackDurationProbe to skip it.
RecordingListing listRecordings(const std::filesystem::path &stickRoot, TrackDurationProbe &durations);

struct RecordingOutcome
{
    enum class Status
    {
        Deleted,       // gone from the stick, and the filesystem agrees
        DeleteFailed,  // still on the stick
        Refused,       // not a recording by the rules above; not touched
        Vanished,      // gone before its delete (or the stick went); nothing deleted it here
        NotAttempted,  // cancelled before its turn
    };
    std::string path;
    std::string fileName;
    std::uint64_t sizeBytes = 0;
    Status status = Status::NotAttempted;
    std::string reason;  // why, for everything but Deleted
};

// Every counter is derived from `outcomes` after the run, and every
// Deleted was asked of the filesystem again after the whole run.
struct RecordingsReport
{
    std::vector<RecordingOutcome> outcomes;  // one per requested path, in request order
    int requested = 0;
    int deleted = 0;
    int failed = 0;  // DeleteFailed + Refused + Vanished
    int notAttempted = 0;
    bool cancelled = false;
};

struct RecordingsProgress
{
    int fileIndex = 0;  // 0-based, of fileCount
    int fileCount = 0;
    std::string fileName;
};

// Test seams. Production passes {}.
struct RecordingDeleteHooks
{
    // Right before an original is deleted: a test takes the file away
    // here, as a stick pulled mid-run would.
    std::function<void(const std::filesystem::path &original)> beforeDelete;
    // Right after removeEntry() said yes: a test puts the file back to
    // see that the filesystem is asked again rather than believed.
    std::function<void(const std::filesystem::path &original)> afterDelete;
    // Stand-in for infrastructure::volumeIdOf(), so a test can have the
    // stick answer differently part-way, as an ejected one does.
    std::function<std::optional<std::uint64_t>(const std::filesystem::path &)> volumeIdOf;
};

// Deletes the given recordings, one file at a time.
//
// Refuses (nothing touched, Refused) any path that is not an audio file
// directly inside a recording folder directly under `stickRoot`. Right
// before each delete the file must still be a plain file and the stick
// must still be the volume it was when the run began; otherwise it is
// Vanished, never Deleted (removeEntry() answers "gone" for a file that
// was never there, so a stick pulled mid-run would otherwise count every
// file after it as deleted). After the delete the filesystem is asked
// whether it is gone, and again for every file once the run is over.
//
// Cancel is checked between files: the file in flight finishes, nothing
// after it is touched, and the report says what was deleted.
RecordingsReport deleteRecordings(const std::filesystem::path &stickRoot, const std::vector<std::string> &recordingPaths,
                                  CancellationToken cancel,
                                  const std::function<void(const RecordingsProgress &)> &progress,
                                  const RecordingDeleteHooks &hooks);

}  // namespace seabass::application
