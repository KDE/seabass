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

// A file's audio through QAudioDecoder (Qt Multimedia's FFmpeg backend,
// the decoder the player and the silence probe use), as the PcmSource
// port hands it to an analyser: one channel, the mean of the file's, as
// floats, sample 0 being the first sample the decoder gives, which is
// Engine's timeline (playback_timeline.hpp).
//
// Decoded as it is read: a buffer is taken from the decoder only when
// the caller has used up the last one, so the samples held at a time are
// a buffer or two and not the track (peakHeldSamples() says how many).
//
// Lives in seabass_audio_qt beside the silence probe, for the same
// reason: seabass_core stays free of Qt. Needs a QCoreApplication in the
// calling thread (it runs a nested QEventLoop) but no GUI.
class QtMultimediaPcmSource : public domain::PcmSource
{
public:
    // Opens the file and decodes up to its first buffer, so that
    // sampleRate() is known when this returns, or error() says why not.
    //
    // `stallTimeoutMs` bounds the wait for any one buffer, not the whole
    // file: a caller may take its time between reads. A damaged file can
    // leave the backend never answering at all, and a scan must not hang
    // on it.
    explicit QtMultimediaPcmSource(const std::string &absoluteFilePath, int stallTimeoutMs = 10000);
    ~QtMultimediaPcmSource() override;
    QtMultimediaPcmSource(const QtMultimediaPcmSource &) = delete;
    QtMultimediaPcmSource &operator=(const QtMultimediaPcmSource &) = delete;

    // Whether this build has a decoding backend that actually loaded
    // (QtMultimediaSilenceProbe::decodingAvailable(), asked the same way).
    static bool decodingAvailable();

    int sampleRate() const override;
    std::size_t read(std::span<float> out) override;
    std::string error() const override;

    // The most decoded samples this source has held at once, waiting to
    // be read.
    std::size_t peakHeldSamples() const;

private:
    struct State;
    std::unique_ptr<State> d;
};

}  // namespace seabass::infrastructure::audio
