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

// What Seabass's own analyser is given of a file: the samples this source
// decodes, mixed to one channel as the mean of the file's, on the
// decoder's timeline, and moved onto rekordbox's timeline afterwards by
// cueTimelineOffsetMs() (playback_timeline.hpp) before a grid is written.
// The analysis ledger records it beside the analyser's own revisions
// (local/analysis_ledger_file.hpp), so a stick can say which tracks were
// analysed from other samples than today's.
//
// RAISE IT by one with any change to what the analyser is given or to
// where its result lands: another decoder or decoder setting, another
// mono mix, a resampling, the timeline shift. Not for a change that
// leaves every sample and every shift as it was (a faster read).
inline constexpr int AnalysisInputRevision = 1;

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
    //
    // A decoder that gives nothing at all before its first sample, and
    // reports no fault, is a fault of the backend's that a sound file
    // meets now and then: the file is then opened again, three times in
    // all (qt_decoder_start.hpp). openAttempts() says how often.
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

    // How often the file was opened: 1 but for the fault above, and 0
    // where it never was (no such file, no backend).
    int openAttempts() const;

private:
    struct State;
    std::unique_ptr<State> d;
};

}  // namespace seabass::infrastructure::audio
