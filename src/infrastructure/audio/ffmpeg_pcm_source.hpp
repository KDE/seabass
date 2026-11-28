// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>

#include "domain/pcm_source.hpp"

namespace seabass::infrastructure::audio
{

// A file's audio through FFmpeg's libraries directly (libavformat,
// libavcodec), as the PcmSource port hands it to an analyser, where the
// build has those libraries (analysis_pcm_source.hpp chooses): one channel, the mean of the file's, as
// floats, sample 0 being the first sample the decoder gives.
//
// Why it exists: QtMultimediaPcmSource decodes through QAudioDecoder,
// whose FFmpeg backend has a race at the start of a decode that Seabass
// cannot cure from outside (qt_decoder_start.hpp, ticket 154): with
// FFmpeg 6 a start now and then gives nothing, which a second start
// cures; with a newer FFmpeg the same race crashes the process. Here
// there is one thread and no resampler: a packet is read, decoded and
// its frame converted in the caller's thread, so there is nothing to
// race with.
//
// What it must give to take the other's place: the same samples, to the
// last one, as QtMultimediaPcmSource for the same file, since both are
// libavcodec with its defaults (an MP3's encoder delay is dropped by the
// decoder in both). ffmpeg_pcm_source_test compares the two over the
// fixtures; a difference there is an AnalysisInputRevision. That holds
// while both are the same FFmpeg: Qt Multimedia's plugin and this link
// the system's one here, and decoders do change between FFmpeg's major
// versions (7.1 has another AAC decoder).
//
// A damaged file is where the two part, and where Qt's is not even the
// same twice: its FLAC decoder runs sixteen threads and what comes out
// around a damaged frame depends on which was how far (a FLAC cut short
// gave 41,472 to 78,336 samples in five runs, with an error; this gives
// 124,416 every time, as `ffmpeg -threads 1` does). A packet the decoder
// refuses is left out and the decode goes on, as a player's does, so
// what follows is earlier by what the packet held; damagedPackets() says
// that this happened. What an analysis should do with such a file
// (take it, warn, refuse) is not decided here.
//
// Needs no QCoreApplication and no event loop.
class FfmpegPcmSource : public domain::PcmSource
{
public:
    // Opens the file and decodes up to its first frame, so that
    // sampleRate() is known when this returns, or error() says why not.
    explicit FfmpegPcmSource(const std::string &absoluteFilePath);
    ~FfmpegPcmSource() override;
    FfmpegPcmSource(const FfmpegPcmSource &) = delete;
    FfmpegPcmSource &operator=(const FfmpegPcmSource &) = delete;

    int sampleRate() const override;
    std::size_t read(std::span<float> out) override;
    std::string error() const override;

    // Packets the decoder refused as damaged and that were left out (the
    // decode goes on after one, as a player's does). Not 0: the audio
    // after the first of them is not where it is in the file.
    int damagedPackets() const;

private:
    struct State;
    std::unique_ptr<State> d;
};

}  // namespace seabass::infrastructure::audio
