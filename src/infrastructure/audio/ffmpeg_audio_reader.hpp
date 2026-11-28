// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace seabass::infrastructure::audio
{

// A file's audio through FFmpeg's libraries directly (libavformat,
// libavcodec), a decoded frame at a time, as floats with the file's own
// channels side by side: what Qt Multimedia's QAudioDecoder hands over
// when asked for floats, without its threads (ffmpeg_pcm_source.hpp says
// why that matters, and what is and is not the same between the two).
//
// One thread, the caller's: a packet is read, decoded and converted when
// next() is called. Needs no QCoreApplication and no event loop.
class FfmpegAudioReader
{
public:
    // Opens the file; error() says why not.
    //
    // `interrupted` is asked while a read of the file is going on, the
    // opening included, and ends it when it says true: next() then has
    // nothing more and error() says the read failed. It is how a caller
    // keeps its timeout and its cancel on a stick that has stopped
    // answering, where a read would otherwise last as long as the
    // system lets it. Called often; it should be quick.
    explicit FfmpegAudioReader(const std::string &absoluteFilePath, std::function<bool()> interrupted = {});

    // The libraries doing the decoding, for a record of which decoder a
    // measurement was made with: "lavc 60.31.102".
    static std::string libraryVersion();
    ~FfmpegAudioReader();
    FfmpegAudioReader(const FfmpegAudioReader &) = delete;
    FfmpegAudioReader &operator=(const FfmpegAudioReader &) = delete;

    struct Chunk
    {
        // frames * channels values: frame 0's channels, then frame 1's.
        // Each a sample rounded to a float, -1 to 1. Good until the next
        // call of next().
        const float *samples = nullptr;
        std::size_t frames = 0;
        int channels = 0;
        int sampleRate = 0;
    };

    // The next decoded frame; none at the end of the audio, or after a
    // failure, when error() is not empty.
    std::optional<Chunk> next();

    std::string error() const;

    // The file's length as its container says it, in ms; 0 where it does
    // not say.
    long long durationMs() const;

    // Packets the decoder refused as damaged and that were left out (the
    // decode goes on after one, as a player's does). Not 0: the audio
    // after the first of them is not where it is in the file.
    int damagedPackets() const;

private:
    struct State;
    std::unique_ptr<State> d;
};

}  // namespace seabass::infrastructure::audio
