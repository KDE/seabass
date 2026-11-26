// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/cover_art_preparer.hpp"
#include "application/ports/recording_encoder.hpp"
#include "application/ports/recording_tag_writer.hpp"

namespace seabass::application
{

// Keep this recording (Own Recordings, step 3): a set the DJ hardware
// recorded onto the stick becomes a tagged FLAC with a cover in a folder
// on the computer.
//
// The order is fixed: the free space on the target's volume, the
// encoding (with the encoder's own check, sample for sample), the cover,
// the tags. The recording on the stick is only ever read: by the encoder,
// and here for its size. A failure or a cancel at any point leaves no
// file of this job in the target folder; the folder itself stays when it
// was made for the job. A file under the target's name is never
// replaced. A job for a recording or a FLAC that another job in this
// process has in hand is refused before it touches anything.

// What the job needs of the file system, apart from what the encoder and
// the tag writer do themselves. Read calls and write calls are apart, so
// a test can prove the recording is never named in a write.
class KeepRecordingFiles
{
public:
    virtual ~KeepRecordingFiles() = default;

    // Reads. A size or a free space nobody can tell is nullopt.
    virtual std::optional<std::uint64_t> fileSize(const std::string &path) = 0;
    // Whether anything (a file, a folder, a link) is at `path`; nullopt
    // when that cannot be told.
    virtual std::optional<bool> exists(const std::string &path) = 0;
    virtual std::optional<std::uint64_t> freeBytes(const std::string &folder) = 0;
    // A picture file's bytes, refused (nullopt) above `maxBytes`.
    virtual std::optional<std::string> readFile(const std::string &path, std::uint64_t maxBytes) = 0;

    // Writes. Each answers "" when it did it, or why not in words.
    // makeFolder: the folder and those above it, whatever of them is
    // missing; a folder that is there already is fine.
    virtual std::string makeFolder(const std::string &folder) = 0;
    // Removes one file this job made.
    virtual std::string removeFile(const std::string &path) = 0;
};

// What is kept, as the form asked for it. Text is UTF-8.
struct KeepRecordingRequest
{
    std::string wavPath;
    std::string folder;
    // As keptRecordingFileName() makes it.
    std::string fileName;
    // The cover is filled from coverPicturePath by the job; an empty
    // path is no cover.
    RecordingTags tags;
    std::string coverPicturePath;
};

enum class KeepRecordingStep
{
    Space,     // the free space on the target's volume
    Encoding,  // the encoder writing the FLAC
    Checking,  // the encoder decoding it again and comparing
    Tagging,   // the cover prepared, the tags and the cover written
};

// "space", "encoding", "checking", "tags": the key the GUI turns into words.
const char *keepRecordingStepKey(KeepRecordingStep step);

struct KeepRecordingProgress
{
    KeepRecordingStep step = KeepRecordingStep::Space;
    // Of the whole job, 0 to 1; never goes back.
    double fraction = 0.0;
};

struct KeepRecordingResult
{
    enum class Status
    {
        Kept,
        Cancelled,
        Failed,
    };
    Status status = Status::Failed;
    // The FLAC, when kept.
    std::string flacPath;
    // Why not, in words for the person who asked; empty when kept or
    // cancelled, unless what the job made could not be taken away again,
    // which is then said here too.
    std::string message;
};

// Room asked for on the target's volume: the FLAC can be as large as the
// recording (noise does not compress), and the tag writer writes a copy
// of it beside it for the time of the write, so twice the recording, and
// a margin for the filesystem.
std::uint64_t keepRecordingSpaceNeeded(std::uint64_t wavBytes);
inline constexpr std::uint64_t KeepRecordingSpaceMargin = 64ULL * 1024 * 1024;

// The largest cover picture read: a photo from a phone is a few MB.
inline constexpr std::uint64_t MaxCoverPictureBytes = 64ULL * 1024 * 1024;

// The FLAC's file name: "Artist - Track name.flac", or the track name
// alone without an artist, made safe for every filesystem the same way
// an imported track's name on the stick is (domain::stickSafePathSegment:
// forbidden characters become '_', control characters go, dots and
// spaces at either end go, a name Windows reserves gets a '_' in front,
// non-ASCII stays). "Recording.flac" when nothing is left.
std::string keptRecordingFileName(std::string_view artist, std::string_view title);

// The track name suggested for a recording: its file name without the
// extension, underscores as spaces.
std::string suggestedRecordingTitle(std::string_view recordingFileName);

// Does the job. Blocks until done; `progress` is called on the calling
// thread. Never throws.
KeepRecordingResult keepRecording(const KeepRecordingRequest &request, KeepRecordingFiles &files,
                                  RecordingEncoder &encoder, CoverArtPreparer &covers, RecordingTagWriter &tags,
                                  const CancellationToken &cancel,
                                  const std::function<void(const KeepRecordingProgress &)> &progress);

}  // namespace seabass::application
