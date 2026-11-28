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

// A PROTOTYPE, not used by the app yet: a file's audio through FFmpeg's
// libraries directly (libavformat, libavcodec), as the PcmSource port
// hands it to an analyser: one channel, the mean of the file's, as
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
// fixtures; a difference there is an AnalysisInputRevision.
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
    // decode goes on after one, as a player's does).
    int damagedPackets() const;

private:
    struct State;
    std::unique_ptr<State> d;
};

}  // namespace seabass::infrastructure::audio
