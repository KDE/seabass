// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <functional>
#include <string>

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
// Blocks until done. Never throws.
class RecordingEncoder
{
public:
    virtual ~RecordingEncoder() = default;

    virtual RecordingEncodeResult encode(const std::string &wavPath, const std::string &flacPath,
                                         const RecordingEncodeOptions &options) = 0;
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
};

}  // namespace seabass::application
