// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "application/ports/cancellation_token.hpp"

namespace seabass::application
{

// How much of the progress encode() reports is the encoding: below this
// share the FLAC is being written, from it on the written file is being
// checked. The check decodes about as fast as the encoding runs.
inline constexpr double RecordingEncodingShare = 0.6;

struct RecordingEncodeOptions
{
    CancellationToken cancel;
    // Called with how much of the whole job is done, 0 to 1, from the
    // thread that called encode(). The encoding and the check of what was
    // written are both in it.
    std::function<void(double)> progress;
    // The person chose to keep the audio of a recording the player did
    // not finish (its header unfinished): every whole frame the file holds
    // after the data chunk's header is encoded, and checked against
    // exactly those. Without it such a recording is refused (unfinished).
    bool keepUnfinished = false;
    // The proven file is not given the target's name: it stays under its
    // temporary name beside the target, and RecordingEncodeResult::unplaced
    // describes it. From then on it is the caller's: placeVerified() gives
    // it the target's name, and a caller that does not place it removes it.
    // Progress then stops short of 1, which is the placing.
    bool leaveUnplaced = false;
};

// A FLAC that was proven to hold a recording, as it is now: where it is,
// its size and its STREAMINFO block (with the MD5 of the samples), so that
// a later step can tell it is still that file.
struct VerifiedFlac
{
    std::string path;
    std::uint64_t size = 0;
    std::array<std::uint8_t, 34> streamInfo{};
};

// What RecordingEncoder::encode did. Exactly one of the three is the
// answer: encoded, cancelled, or an error.
struct RecordingEncodeResult
{
    bool encoded = false;
    bool cancelled = false;
    // Why not, in words for the person who asked. Empty when encoded or
    // cancelled.
    std::string error;
    // What the recording is, as the WAV's header says: known once the
    // WAV was read, whatever came after.
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    // Frames (one sample of every channel) in the WAV, and the frames the
    // written file was decoded to and found equal to the WAV's, one by
    // one. Equal when encoded.
    std::uint64_t framesInWav = 0;
    std::uint64_t framesVerified = 0;
    // The WAV's header was not completed (a data size of 0 or 0xFFFFFFFF,
    // or more than the file holds). When encoded, framesInWav is the whole
    // frames the file held and partialFrameBytesDropped the bytes of a
    // last, partial frame that were left out; when refused for it,
    // keepUnfinished was not set, and framesInWav is what the file holds.
    // framesDeclared is what the header declares, in whole frames, and
    // nullopt when it declares no size (the file is then read to its end).
    bool unfinished = false;
    std::uint64_t partialFrameBytesDropped = 0;
    std::optional<std::uint64_t> framesDeclared;
    // Of a header that declares more than the file holds: how many bytes
    // less the file holds than it declares, and whether that length is
    // one a recording can have (declaredUnusable when it is not, and
    // framesDeclared is then not to be said as a length).
    std::uint64_t bytesMissing = 0;
    bool declaredUnusable = false;
    // When encoded with leaveUnplaced: the proven file under its temporary
    // name. Empty otherwise.
    VerifiedFlac unplaced;
    // Things the person should know of an encoding that succeeded, in
    // words (a second name of the FLAC that could not be removed).
    std::vector<std::string> notes;
};

// What RecordingEncoder::placeVerified did.
struct RecordingPlaceResult
{
    bool placed = false;
    // Why not, in words; empty when placed.
    std::string error;
    // Not placed although the rename was done: what has the target's name
    // afterwards is not the size of the file that was checked. It is left
    // where it is, and does not count as kept. When false and not placed,
    // the file is still under its own name.
    bool leftUnderTargetName = false;
    // Things the person should know of a placing that succeeded (a second
    // name of the FLAC that could not be removed).
    std::vector<std::string> notes;
};

// Encodes a recording, a PCM WAV as a DJ player writes it, to a lossless
// FLAC, for keeping it.
//
// The FLAC counts only once it was decoded again and found to hold the
// WAV's samples, every one of every channel, and as many of them. Until
// then it lies under a temporary name beside the target, and any failure,
// a mismatch or a cancel takes that away again: a file under the target's
// name is always a whole and proven one. A target that exists is refused
// and never replaced.
//
// encode() places the proven file itself, unless asked to leave it
// unplaced (RecordingEncodeOptions::leaveUnplaced), for a caller that
// writes into it first (the tags) and then has it placed by
// placeVerified(). Both place the same way.
//
// Blocks until done. Never throws.
class RecordingEncoder
{
public:
    virtual ~RecordingEncoder() = default;

    virtual RecordingEncodeResult encode(const std::string &wavPath, const std::string &flacPath,
                                         const RecordingEncodeOptions &options) = 0;

    // Gives a proven file, as `file` describes it now, the name `flacPath`:
    // the file is flushed without being created (one that went away is a
    // failure, never an empty FLAC), its size and STREAMINFO block are
    // compared with `file` once more, and it is renamed by a rename that
    // never replaces. The target's size is read again after the rename.
    // On a failure before the rename the file stays under its own name.
    virtual RecordingPlaceResult placeVerified(const VerifiedFlac &file, const std::string &flacPath) = 0;
};

// For builds without an encoder: every encode fails, saying so.
class NullRecordingEncoder : public RecordingEncoder
{
public:
    RecordingEncodeResult encode(const std::string &, const std::string &, const RecordingEncodeOptions &) override
    {
        RecordingEncodeResult result;
        result.error = "this build of Seabass has no FLAC encoder";
        return result;
    }
    RecordingPlaceResult placeVerified(const VerifiedFlac &, const std::string &) override
    {
        RecordingPlaceResult result;
        result.error = "this build of Seabass has no FLAC encoder";
        return result;
    }
};

}  // namespace seabass::application
