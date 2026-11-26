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
// recorder would otherwise add to a name it does not know. The whole name
// is "<target's name>.<16 hex digits>.seabass-encode-tmp.flac", a name of
// each job's own (uniqueTempBeside()), so that two jobs never write or
// remove each other's file. The encoder removes only its own; a leftover
// of a job that was killed stays for a scan.
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
// The checked file is then flushed without being created (a temporary
// file that went away is a failure, never an empty FLAC), looked at again
// (its size and its STREAMINFO block, with the MD5 of the samples, must be
// those that were checked), and renamed to the target by a rename that
// never replaces (renameNoReplace()). The target's size is read once more
// after the rename.
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

    // For tests: called with the path of the temporary file at three
    // points, to change the world there and see the encoder refuse.
    struct TestHooks
    {
        // Before the check: to damage the file.
        std::function<void(const std::string &)> beforeCheck;
        // After the check, before the file is flushed and looked at again.
        std::function<void(const std::string &)> beforePlace;
        // After the last look at the target, right before the rename.
        std::function<void(const std::string &)> beforeRename;
    };
    void setBeforeCheckForTests(std::function<void(const std::string &)> hook) { m_hooks.beforeCheck = std::move(hook); }
    void setBeforePlaceForTests(std::function<void(const std::string &)> hook) { m_hooks.beforePlace = std::move(hook); }
    void setBeforeRenameForTests(std::function<void(const std::string &)> hook)
    {
        m_hooks.beforeRename = std::move(hook);
    }

private:
    int m_stallTimeoutMs;
    TestHooks m_hooks;
};

}  // namespace seabass::infrastructure::audio
