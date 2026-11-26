// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
#include <string>

#include "application/ports/recording_encoder.hpp"

namespace seabass::infrastructure::audio
{

// The suffix of the file a FLAC is written to before it is proven, for a
// scan that tidies leftovers. It ends in .flac, the extension the
// recorder would otherwise add to a name it does not know.
inline constexpr const char *RecordingEncodeTempSuffix = ".seabass-encode-tmp.flac";

// RecordingEncoder through Qt Multimedia: the WAV's samples go into a
// QMediaRecorder through a QAudioBufferInput (the FFmpeg backend's FLAC
// encoder, no other library), and come back out of a QAudioDecoder for
// the check.
//
// Piece by piece both ways: the WAV is read a piece at a time as the
// recorder takes it, and the check decodes the FLAC a buffer at a time and
// compares each with the same frames read again from the WAV, so a
// recording of hours is never in memory.
//
// Needs a QCoreApplication; runs a nested QEventLoop in the calling
// thread, which should be a worker thread (QtMultimediaPcmSource does the
// same). No GUI.
//
// The recorder's own word is not taken for anything: it did not stop by
// itself at the end of the audio, it holds back the last encoder block
// until it is stopped, and it refuses most buffers while its queue is
// full. So every wait on it is bounded by `stallTimeoutMs` without any
// sign of progress, the recorder is stopped once it has reported all but
// that last block (or has been quiet for a while), and the end of the job
// is the decoded check, which also proves the stop lost nothing.
//
// 24 bit recordings are handed over in Qt's 32 bit sample format (Qt has
// no 24 bit one), and FFmpeg's FLAC encoder writes them as 24 bit. Measured
// bit for bit with Qt 6.11.1 and FFmpeg 6.1: the check compares every bit
// of every sample, and the STREAMINFO block must say 24 bit, so a Qt or
// FFmpeg that rounds them to 16 bit fails the encode, never writes a
// lossy file.
class QtFlacRecordingEncoder : public application::RecordingEncoder
{
public:
    explicit QtFlacRecordingEncoder(int stallTimeoutMs = 15000);

    // Whether this build has a backend that both decodes and encodes FLAC
    // (asks Qt the way QtMultimediaPcmSource::decodingAvailable() does).
    static bool encodingAvailable();

    application::RecordingEncodeResult encode(const std::string &wavPath, const std::string &flacPath,
                                              const application::RecordingEncodeOptions &options) override;

    // For tests: called with the path of the encoded file before it is
    // checked, to damage it and see the check refuse it.
    void setBeforeCheckForTests(std::function<void(const std::string &)> hook) { m_beforeCheck = std::move(hook); }

private:
    int m_stallTimeoutMs;
    std::function<void(const std::string &)> m_beforeCheck;
};

}  // namespace seabass::infrastructure::audio
