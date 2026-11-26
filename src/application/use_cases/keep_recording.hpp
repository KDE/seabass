// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
// and here for its size. A failure or a cancel before the tags were
// written and read back leaves no file of this job in the target folder,
// with one exception that is always said (KeepRecordingResult::leftBehind):
// a file under the target's name that the job cannot prove is its own
// unfinished FLAC is left where it is. A cancel that comes after the tags
// were written and read back changes nothing: the job is done, and the
// FLAC is kept. The folder itself stays when it was made for the job. A
// file under the target's name is never replaced. A job for a recording
// or a FLAC that another job in this process has in hand is refused
// before it touches anything.
//
// The recording must be on the stick named (stickRoot), and the target
// folder is never on the stick the recording is on (compared
// as files: links followed, letters as the filesystem compares them, the
// nearest folder that is there for one still to be made). The recordings
// page lists every audio file in the stick's recording folders, so a FLAC
// kept beside its WAV would be offered for deletion with it.

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
    // Whether `path` is `folder` or inside it, compared as files: both
    // resolved, links followed, letters compared as the filesystem
    // compares them. A `path` that is not there (a folder still to be
    // made) counts by the nearest folder above it that is. nullopt when
    // that cannot be told, `folder` not being there included.
    virtual std::optional<bool> isWithin(const std::string &path, const std::string &folder) = 0;
    // The longest file name, in bytes of UTF-8, the filesystem that holds
    // `folder` takes (the nearest folder above it that is there, for one
    // still to be made); nullopt when it cannot be told.
    virtual std::optional<std::uint64_t> maxNameBytes(const std::string &folder) = 0;

    // Writes. Each answers "" when it did it, or why not in words.
    // makeFolder: the folder and those above it, whatever of them is
    // missing; a folder that is there already is fine.
    virtual std::string makeFolder(const std::string &folder) = 0;
    // Removes one file this job made.
    virtual std::string removeFile(const std::string &path) = 0;
    // Removes Seabass's own leftover temporary files in `folder` (not
    // below it, links never followed): regular files named as the
    // encoder and the tag writer name theirs, older than a day, whose
    // FLAC's name `inHand` does not claim for a job of this process.
    // Answers a sentence per file removed, or found and not removable.
    virtual std::vector<std::string>
    sweepLeftovers(const std::string &folder, const std::function<bool(const std::string &flacName)> &inHand) = 0;
};

// How old a leftover temporary file must be before a keep job removes it:
// a younger one may be another Seabass's, writing right now.
inline constexpr int KeepRecordingLeftoverAgeHours = 24;

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
    // The root of the stick the recording is on: the folder must not be
    // on it. Required.
    std::string stickRoot;
    // How many bytes the longest temporary name the encoder and the tag
    // writer give beside the FLAC adds to its name: the name and that
    // must fit the filesystem's longest name.
    std::size_t tempNameExtraBytes = 0;
    // The person chose to keep the audio of a recording the player did
    // not finish (RecordingEncodeOptions::keepUnfinished). Without it such
    // a recording is refused.
    bool keepUnfinished = false;
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
    // cancelled, unless a file was left in the folder, which is then said
    // here too, with its path.
    std::string message;
    // When not kept: a file under the FLAC's name that was left in the
    // folder (one that could not be removed, or one that is not provably
    // the job's own unfinished FLAC). Empty when nothing is there; the
    // message says so when that could not be told.
    std::string leftBehind;
    // Things to know about the job, kept or not, a sentence each: the
    // recording was not finished by the player and how much of it was
    // kept, a leftover temporary file of an earlier job that was removed.
    std::vector<std::string> notes;
    // The player did not finish the recording; when kept, the FLAC holds
    // keptSeconds of audio, every whole frame the file held.
    bool unfinished = false;
    double keptSeconds = 0.0;
};

// The longest name a filesystem gives a file, in bytes: ext4, btrfs,
// XFS, APFS and ZFS count UTF-8 bytes, NTFS, FAT and exFAT count UTF-16
// units, of which a name never has more than its UTF-8 bytes.
inline constexpr std::size_t MaxFileNameBytes = 255;

// Whether a tag's text holds a character a tag field cannot: one below
// U+0020, U+007F, U+0085, or a Unicode line or paragraph separator
// (U+2028, U+2029). Text is UTF-8.
bool hasControlCharacter(std::string_view text);

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
//
// The part before ".flac" is at most `maxStemBytes` bytes of UTF-8, cut
// where `cut` says: it answers, for a UTF-8 text and a number of bytes,
// the longest start of the text of at most that many bytes that ends on
// a boundary between two user-perceived characters (never inside a UTF-8
// sequence, a combining sequence or an emoji sequence). The GUI answers
// it with QTextBoundaryFinder.
using GraphemeCut = std::function<std::size_t(std::string_view text, std::size_t maxBytes)>;
std::string keptRecordingFileName(std::string_view artist, std::string_view title, std::size_t maxStemBytes,
                                  const GraphemeCut &cut);

// The room for the part before ".flac" in a folder whose filesystem takes
// names of up to `maxNameBytes`, when temporary names add
// `tempNameExtraBytes` to the FLAC's name. 0 when there is none.
std::size_t keptRecordingStemBytes(std::size_t maxNameBytes, std::size_t tempNameExtraBytes);

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
