// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/qt_flac_recording_encoder.hpp"

#include <QAudioBuffer>
#include <QAudioBufferInput>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QMediaCaptureSession>
#include <QMediaFormat>
#include <QMediaRecorder>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <bit>
#include <cstring>
#include <exception>
#include <optional>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

#include "infrastructure/audio/flac_stream_info.hpp"
#include "infrastructure/audio/recording_file_steps.hpp"
#include "infrastructure/audio/wav_pcm_file.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/long_paths.hpp"
#include "infrastructure/no_replace_rename.hpp"
#include "infrastructure/paths/utf8_path.hpp"

namespace seabass::infrastructure::audio
{

namespace
{

namespace fs = std::filesystem;

// Frames handed to the recorder at a time, and compared at a time in the
// check: a third of a second at 48 kHz, 128 KB of 24 bit stereo.
constexpr std::uint64_t FramesPerPiece = 16384;
// How much of the progress the encoding is; the check is the rest.
constexpr double EncodingShare = application::RecordingEncodingShare;
// What the recorder may hold back of the end of the audio until it is
// stopped: FFmpeg's FLAC encoder takes blocks of about 105 ms, and the
// recorder reports its duration in whole milliseconds.
constexpr qint64 HeldBackTailMs = 250;
// How long a recorder that has all the audio may stay without reporting
// more of it before it is stopped anyway.
constexpr int QuietStopMs = 500;
// How often the event loop looks at the cancellation token, which is
// flipped from another thread.
constexpr int CancelPollMs = 50;
// The most the check reports: the whole job (1) is reported only once
// the FLAC has its name.
constexpr double LastCheckProgress = 0.999;

QString qtPath(const fs::path &path)
{
    return QString::fromStdString(pathToUtf8(path));
}

// What the recorder is given: 16 bit samples as they are, 24 bit ones in
// the top three bytes of a 32 bit sample (Qt has no 24 bit format), which
// is how FFmpeg's FLAC encoder takes them.
QAudioFormat recorderFormat(const WavLayout &wav)
{
    QAudioFormat format;
    format.setSampleRate(wav.sampleRate);
    format.setChannelCount(wav.channels);
    format.setSampleFormat(wav.bitsPerSample == 16 ? QAudioFormat::Int16 : QAudioFormat::Int32);
    return format;
}

void toRecorderSamples(const std::vector<std::byte> &wavBytes, const WavLayout &wav, QByteArray &out)
{
    if (wav.bitsPerSample == 16) {
        out = QByteArray(reinterpret_cast<const char *>(wavBytes.data()), static_cast<qsizetype>(wavBytes.size()));
        return;
    }
    const std::size_t samples = wavBytes.size() / 3;
    out = QByteArray(static_cast<qsizetype>(samples * 4), Qt::Uninitialized);
    char *to = out.data();
    const auto *from = reinterpret_cast<const char *>(wavBytes.data());
    for (std::size_t i = 0; i < samples; ++i) {
        to[4 * i] = 0;
        to[4 * i + 1] = from[3 * i];
        to[4 * i + 2] = from[3 * i + 1];
        to[4 * i + 3] = from[3 * i + 2];
    }
}

// A WAV sample on the scale of 32 bits, where samples of every width can
// be compared without losing a bit.
std::int32_t wavSample32(const std::byte *p, int bits)
{
    const auto b = [p](int i) { return std::to_integer<std::uint32_t>(p[i]); };
    if (bits == 16) {
        return static_cast<std::int32_t>((b(0) << 16) | (b(1) << 24));
    }
    return static_cast<std::int32_t>((b(0) << 8) | (b(1) << 16) | (b(2) << 24));
}

// The first sample of `buffer` that is not the WAV's at the same place,
// as an index into both; -1 when all are equal; nullopt when the buffer's
// format cannot be compared at full precision.
std::optional<qsizetype> firstDifference(const QAudioBuffer &buffer, const std::byte *wav, qsizetype samples, int bits)
{
    const int bytesPerSample = bits / 8;
    switch (buffer.format().sampleFormat()) {
    case QAudioFormat::Int16: {
        const qint16 *decoded = buffer.constData<qint16>();
        // The WAV's bytes are the decoded samples on a little endian host.
        if (bits == 16 && std::endian::native == std::endian::little
            && std::memcmp(decoded, wav, static_cast<std::size_t>(samples) * 2) == 0) {
            return -1;
        }
        for (qsizetype i = 0; i < samples; ++i) {
            const auto value = static_cast<std::int32_t>(static_cast<std::uint32_t>(decoded[i]) << 16);
            if (value != wavSample32(wav + i * bytesPerSample, bits)) {
                return i;
            }
        }
        return -1;
    }
    case QAudioFormat::Int32: {
        const qint32 *decoded = buffer.constData<qint32>();
        for (qsizetype i = 0; i < samples; ++i) {
            if (decoded[i] != wavSample32(wav + i * bytesPerSample, bits)) {
                return i;
            }
        }
        return -1;
    }
    case QAudioFormat::UInt8:
    case QAudioFormat::Float:
    case QAudioFormat::Unknown:
    case QAudioFormat::NSampleFormats:
        break;
    }
    return std::nullopt;
}

class EncodeJob
{
public:
    EncodeJob(const fs::path &wavPath, const fs::path &target, const application::RecordingEncodeOptions &options,
              int stallTimeoutMs, const QtFlacRecordingEncoder::TestHooks &hooks)
        : m_wavPath(wavPath)
        , m_target(target)
        , m_temp(uniqueTempBeside(target, RecordingEncodeTempSuffix))
        , m_options(options)
        , m_stallTimeoutMs(stallTimeoutMs)
        , m_hooks(hooks)
    {
        m_wavName = pathToUtf8(wavPath.filename());
    }

    application::RecordingEncodeResult run()
    {
        if (!prepare()) {
            return m_result;
        }
        bool done = false;
        try {
            done = encodeToTemp() && verifyTemp() && place();
        } catch (const std::exception &e) {
            fail(e.what());
        } catch (...) {
            fail("unknown error");
        }
        if (!done) {
            discardTemp();
            if (m_cancelled) {
                application::RecordingEncodeResult cancelled;
                cancelled.cancelled = true;
                copyLayoutInto(cancelled);
                return cancelled;
            }
            m_result.encoded = false;
            return m_result;
        }
        m_result.encoded = true;
        m_result.error.clear();
        // The FLAC is in its place and proven: a progress callback that
        // throws now does not make it a failure.
        try {
            report(1.0);
        } catch (...) {
        }
        return m_result;
    }

private:
    fs::path m_wavPath;
    fs::path m_target;
    fs::path m_temp;
    std::string m_wavName;
    application::RecordingEncodeOptions m_options;
    int m_stallTimeoutMs;
    QtFlacRecordingEncoder::TestHooks m_hooks;
    std::unique_ptr<WavPcmFile> m_wav;
    fs::file_time_type m_wavModified{};
    QByteArray m_wavMd5;
    // The file the check proved: its size and its STREAMINFO block (with
    // the MD5 of its samples), as read before it was decoded.
    std::uint64_t m_checkedSize = 0;
    FlacStreamInfo m_checkedInfo;
    application::RecordingEncodeResult m_result;
    bool m_cancelled = false;
    bool m_tempMayExist = false;
    double m_lastProgress = 0.0;

    void copyLayoutInto(application::RecordingEncodeResult &result) const
    {
        result.sampleRate = m_result.sampleRate;
        result.channels = m_result.channels;
        result.bitsPerSample = m_result.bitsPerSample;
        result.framesInWav = m_result.framesInWav;
        result.unfinished = m_result.unfinished;
        result.partialFrameBytesDropped = m_result.partialFrameBytesDropped;
        result.framesDeclared = m_result.framesDeclared;
    }

    void report(double fraction)
    {
        fraction = std::clamp(fraction, m_lastProgress, 1.0);
        m_lastProgress = fraction;
        if (m_options.progress) {
            m_options.progress(fraction);
        }
    }

    bool refuse(const std::string &why)
    {
        m_result.error = m_wavName + " cannot be encoded: " + why;
        return false;
    }

    bool fail(const std::string &why)
    {
        if (m_result.error.empty()) {
            m_result.error = m_wavName + " was not encoded: " + why;
        }
        return false;
    }

    // Everything that can be known before the recorder is started.
    bool prepare()
    {
        if (QCoreApplication::instance() == nullptr) {
            return refuse("there is no QCoreApplication, and the encoder needs an event loop");
        }
        std::error_code ec;
        m_wavModified = fs::last_write_time(longPathSafe(m_wavPath), ec);
        if (ec) {
            return refuse("it could not be read");
        }
        // One reading for the encoding and for the check: the same object
        // reads the frames for both, so the FLAC is compared with exactly
        // the frames that were encoded, and STREAMINFO's total must be
        // their number.
        m_wav = std::make_unique<WavPcmFile>(
            m_wavPath, m_options.keepUnfinished ? WavReading::ToEndOfFile : WavReading::AsDeclared);
        if (const std::optional<WavUnfinished> &unfinished = m_wav->unfinished()) {
            m_result.unfinished = true;
            m_result.framesInWav = unfinished->framesHeld;
            m_result.partialFrameBytesDropped = unfinished->partialFrameBytes;
            if (unfinished->kind != WavUnfinished::Kind::NoSize) {
                m_result.framesDeclared = unfinished->framesDeclared;
            }
            // The format is known: a refusal for it can say how long.
            m_result.sampleRate = m_wav->layout().sampleRate;
            m_result.channels = m_wav->layout().channels;
            m_result.bitsPerSample = m_wav->layout().bitsPerSample;
        }
        if (!m_wav->isOpen()) {
            return refuse(m_wav->error());
        }
        const WavLayout &layout = m_wav->layout();
        m_result.sampleRate = layout.sampleRate;
        m_result.channels = layout.channels;
        m_result.bitsPerSample = layout.bitsPerSample;
        m_result.framesInWav = layout.frames;

        const std::string targetName = pathToUtf8(m_target.filename());
        const std::string tempName = pathToUtf8(m_temp.filename());
        // The recording is only ever read: neither the FLAC nor the
        // temporary file may be it, under any spelling of its name. A
        // question the filesystem cannot answer is refused with its
        // reason, never taken as an answer.
        std::error_code lookedAt;
        const std::optional<bool> targetIsWav = sameFile(m_target, m_wavPath, &lookedAt);
        if (!targetIsWav) {
            return refuse(lookError(targetName, lookedAt));
        }
        const std::optional<bool> tempIsWav = sameFile(m_temp, m_wavPath, &lookedAt);
        if (!tempIsWav) {
            return refuse(lookedAt == std::errc::filename_too_long
                              ? "the temporary name " + tempName + " beside " + targetName
                                  + " is too long for this drive. Make the name shorter"
                              : lookError(tempName, lookedAt));
        }
        if (*targetIsWav || *tempIsWav) {
            return refuse(targetName + " would be the recording itself");
        }
        const std::optional<bool> there = somethingAt(m_target, &lookedAt);
        if (!there) {
            return refuse(lookError(targetName, lookedAt));
        }
        if (*there) {
            return refuse(targetName + " exists already, and is not replaced");
        }
        const fs::path folder = m_target.parent_path();
        if (!fs::is_directory(longPathSafe(folder), ec)) {
            return refuse("the folder " + pathToUtf8(folder) + " cannot be written to: it is not a folder");
        }
        // The temporary name is this job's own, so nothing is there; a
        // leftover of an earlier run has another name and stays for a
        // scan that tidies them.
        if (somethingAt(m_temp) != std::optional<bool>(false)) {
            return refuse("something is in the way of its temporary file " + pathToUtf8(m_temp.filename()));
        }
        if (QMediaFormat(QMediaFormat::FLAC).isSupported(QMediaFormat::Encode) == false) {
            return refuse("this system's Qt Multimedia cannot encode FLAC");
        }
        return true;
    }

    // Bounded waiting: runs `loop` until it is quit, with a stall timer
    // the caller restarts on every sign of progress and a poll of the
    // cancellation token.
    struct Waiter
    {
        QEventLoop loop;
        QTimer stall;
        QTimer cancelPoll;
        bool stalled = false;
        bool cancelled = false;
        bool done = false;

        Waiter(int stallTimeoutMs, const application::CancellationToken &cancel)
        {
            stall.setSingleShot(true);
            stall.setInterval(stallTimeoutMs);
            QObject::connect(&stall, &QTimer::timeout, &loop, [this]() {
                stalled = true;
                finish();
            });
            cancelPoll.setInterval(CancelPollMs);
            QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [this, cancel]() {
                if (cancel.cancelled()) {
                    cancelled = true;
                    finish();
                }
            });
        }

        void alive()
        {
            if (!done) {
                stall.start();
            }
        }

        // Also before run(): a signal may come straight out of the call
        // that started the work, and then the loop is not entered at all.
        // The timers stop here, so that a finished waiter cannot fire
        // inside the next one's loop.
        void finish()
        {
            done = true;
            stall.stop();
            cancelPoll.stop();
            loop.quit();
        }

        void run()
        {
            if (done) {
                return;
            }
            stall.start();
            cancelPoll.start();
            loop.exec();
            stall.stop();
            cancelPoll.stop();
        }
    };

    bool encodeToTemp()
    {
        const WavLayout &layout = m_wav->layout();
        const QAudioFormat format = recorderFormat(layout);
        QMediaCaptureSession session;
        QAudioBufferInput input(format);
        QMediaRecorder recorder;
        session.setAudioBufferInput(&input);
        session.setRecorder(&recorder);
        QMediaFormat mediaFormat(QMediaFormat::FLAC);
        mediaFormat.setAudioCodec(QMediaFormat::AudioCodec::FLAC);
        recorder.setMediaFormat(mediaFormat);
        recorder.setAudioSampleRate(layout.sampleRate);
        recorder.setAudioChannelCount(layout.channels);
        recorder.setOutputLocation(QUrl::fromLocalFile(qtPath(m_temp)));
        // Without it the recorder took no buffer at all (the spike).
        recorder.setAutoStop(true);

        Waiter waiter(m_stallTimeoutMs, m_options.cancel);
        std::string error;
        QCryptographicHash md5(QCryptographicHash::Md5);
        std::vector<std::byte> wavBytes;
        QByteArray pending;
        std::uint64_t pendingFirst = 0;
        std::uint64_t nextFrame = 0;
        bool allSent = false;
        bool stopAsked = false;
        // The recorder's duration once the whole WAV is in, in its unit,
        // less the tail it holds back: the encoder takes whole blocks, and
        // the last, shorter one is encoded only when the recorder stops.
        const qint64 expectedMs = static_cast<qint64>(layout.frames * 1000 / static_cast<std::uint64_t>(layout.sampleRate));
        const qint64 stopAtMs = expectedMs - HeldBackTailMs;

        // A recording shorter than the tail, or a recorder that reports
        // no duration: stopped once it has been quiet for a while.
        QTimer quiet;
        quiet.setSingleShot(true);
        quiet.setInterval(QuietStopMs);
        const auto stop = [&]() {
            if (!stopAsked) {
                stopAsked = true;
                quiet.stop();
                recorder.stop();
            }
        };
        QObject::connect(&quiet, &QTimer::timeout, &waiter.loop, stop);
        const auto stopWhenDone = [&]() {
            // The recorder did not stop by itself at the end (the spike),
            // so it is told to once it has encoded all but the tail. Not
            // before it has encoded anything: stopped that early, it wrote
            // no file at all. Had the stop lost anything, the check finds
            // it.
            if (!allSent || stopAsked) {
                return;
            }
            if (recorder.duration() > 0 && recorder.duration() >= stopAtMs) {
                stop();
            } else {
                quiet.start();
            }
        };
        const auto pump = [&]() {
            if (waiter.done || allSent) {
                return;
            }
            try {
                for (;;) {
                    if (m_options.cancel.cancelled()) {
                        waiter.cancelled = true;
                        waiter.finish();
                        return;
                    }
                    if (pending.isEmpty()) {
                        if (nextFrame >= layout.frames) {
                            allSent = true;
                            // The empty buffer is the end of the stream
                            // to a recorder that stops by itself.
                            input.sendAudioBuffer(QAudioBuffer());
                            stopWhenDone();
                            return;
                        }
                        const std::uint64_t frames = std::min(FramesPerPiece, layout.frames - nextFrame);
                        wavBytes.resize(static_cast<std::size_t>(frames) * static_cast<std::size_t>(layout.blockAlign));
                        m_wav->readFrames(nextFrame, wavBytes);
                        md5.addData(QByteArrayView(reinterpret_cast<const char *>(wavBytes.data()),
                                                   static_cast<qsizetype>(wavBytes.size())));
                        toRecorderSamples(wavBytes, layout, pending);
                        pendingFirst = nextFrame;
                        nextFrame += frames;
                    }
                    const qint64 startUs = static_cast<qint64>(pendingFirst * 1000000
                                                               / static_cast<std::uint64_t>(layout.sampleRate));
                    // Refused while the recorder's queue is full: kept,
                    // and sent again when it says it is ready.
                    if (!input.sendAudioBuffer(QAudioBuffer(pending, format, startUs))) {
                        return;
                    }
                    pending.clear();
                    waiter.alive();
                    report(EncodingShare * static_cast<double>(nextFrame) / static_cast<double>(layout.frames));
                }
            } catch (const std::exception &e) {
                error = std::string("the WAV could not be read: ") + e.what();
                waiter.finish();
            }
        };

        QObject::connect(&input, &QAudioBufferInput::readyToSendAudioBuffer, &waiter.loop, pump);
        QObject::connect(&recorder, &QMediaRecorder::durationChanged, &waiter.loop, [&](qint64) {
            waiter.alive();
            stopWhenDone();
        });
        QObject::connect(&recorder, &QMediaRecorder::errorOccurred, &waiter.loop,
                         [&](QMediaRecorder::Error, const QString &message) {
                             if (error.empty()) {
                                 error = "the encoder failed: " + message.toStdString();
                             }
                             waiter.finish();
                         });
        QObject::connect(&recorder, &QMediaRecorder::recorderStateChanged, &waiter.loop,
                         [&](QMediaRecorder::RecorderState state) {
                             waiter.alive();
                             if (state == QMediaRecorder::StoppedState) {
                                 waiter.finish();
                             }
                         });

        m_tempMayExist = true;
        recorder.record();
        if (recorder.error() != QMediaRecorder::NoError && error.empty()) {
            error = "the encoder failed: " + recorder.errorString().toStdString();
            waiter.finish();
        }
        // A recorder that never says it is ready is given the first
        // buffer anyway; it refuses what it cannot take.
        QTimer::singleShot(0, &waiter.loop, pump);
        waiter.run();

        const bool stoppedByItself = recorder.recorderState() == QMediaRecorder::StoppedState;
        if (!stoppedByItself) {
            // Whatever ended the wait, the recorder lets go of the file
            // before it is looked at or taken away.
            Waiter stopping(m_stallTimeoutMs, application::CancellationToken::none());
            QObject::connect(&recorder, &QMediaRecorder::recorderStateChanged, &stopping.loop,
                             [&](QMediaRecorder::RecorderState state) {
                                 if (state == QMediaRecorder::StoppedState) {
                                     stopping.finish();
                                 }
                             });
            recorder.stop();
            if (recorder.recorderState() != QMediaRecorder::StoppedState) {
                stopping.run();
            }
            if (stopping.stalled && error.empty()) {
                error = "the encoder did not stop within " + std::to_string(m_stallTimeoutMs) + " ms";
            }
        }
        if (waiter.cancelled) {
            m_cancelled = true;
            return false;
        }
        if (!error.empty()) {
            return fail(error);
        }
        if (waiter.stalled) {
            return fail("the encoder gave no sign of progress for " + std::to_string(m_stallTimeoutMs) + " ms");
        }
        if (recorder.error() != QMediaRecorder::NoError) {
            return fail("the encoder failed: " + recorder.errorString().toStdString());
        }
        if (!allSent) {
            return fail("the encoder stopped before it was given the whole recording");
        }
        const QString actual = recorder.actualLocation().toLocalFile();
        if (!actual.isEmpty()) {
            // Compared as files: another spelling of the same path (a UNC
            // path, "..") is the temporary file all the same.
            const fs::path actualPath = pathFromUtf8(actual.toStdString());
            const std::optional<bool> tempThere = somethingAt(m_temp);
            std::error_code ec;
            const bool same = tempThere == std::optional<bool>(true)
                && fs::equivalent(longPathSafe(actualPath), longPathSafe(m_temp), ec) && !ec;
            if (!same) {
                // The recorder wrote somewhere else. That file is its own
                // and goes as well, unless it is a file that was there
                // before: the recording or the target.
                if (fs::symlink_status(longPathSafe(actualPath), ec).type() == fs::file_type::regular
                    && sameFile(actualPath, m_wavPath) == std::optional<bool>(false)
                    && sameFile(actualPath, m_target) == std::optional<bool>(false)) {
                    fs::remove(longPathSafe(actualPath), ec);
                }
                return fail("the encoder wrote to " + actual.toStdString() + " and not where it was asked to");
            }
        }
        m_wavMd5 = md5.result();
        return true;
    }

    // Decodes the temporary file and compares it with the WAV, sample for
    // sample, every channel, and the number of frames.
    bool verifyTemp()
    {
        if (m_hooks.beforeCheck) {
            m_hooks.beforeCheck(pathToUtf8(m_temp));
        }
        const WavLayout &layout = m_wav->layout();
        const std::optional<std::uint64_t> size = fileSizeOf(m_temp);
        const std::optional<FlacStreamInfo> info = readFlacStreamInfo(m_temp);
        if (!info || !size) {
            return fail("what the encoder wrote is not a FLAC file");
        }
        m_checkedSize = *size;
        m_checkedInfo = *info;
        if (info->sampleRate != layout.sampleRate || info->channels != layout.channels
            || info->bitsPerSample != layout.bitsPerSample) {
            return fail("the encoder wrote " + std::to_string(info->bitsPerSample) + " bit, "
                        + std::to_string(info->channels) + " channel audio at " + std::to_string(info->sampleRate)
                        + " Hz, not the recording's " + std::to_string(layout.bitsPerSample) + " bit, "
                        + std::to_string(layout.channels) + " channel audio at " + std::to_string(layout.sampleRate)
                        + " Hz");
        }
        // The frames read: for a recording the player did not finish, the
        // whole frames the file holds, which is what was kept.
        if (info->totalFrames != layout.frames) {
            return fail("the encoded file says it holds " + std::to_string(info->totalFrames) + " frames, the WAV holds "
                        + std::to_string(layout.frames));
        }
        // The FLAC's own MD5 is of the samples as the WAV stores them.
        // Zero is an encoder that did not compute it; the decoding below
        // is the proof either way.
        const bool md5Given = std::any_of(info->md5.begin(), info->md5.end(), [](std::uint8_t b) { return b != 0; });
        if (md5Given
            && (m_wavMd5.size() != 16 || std::memcmp(m_wavMd5.constData(), info->md5.data(), 16) != 0)) {
            return fail("the encoded file's MD5 is not that of the recording's samples");
        }

        QAudioDecoder decoder;
        if (!decoder.isSupported()) {
            return fail("there is no audio decoder to check the encoded file with");
        }
        decoder.setSource(QUrl::fromLocalFile(qtPath(m_temp)));
        Waiter waiter(m_stallTimeoutMs, m_options.cancel);
        std::string error;
        std::uint64_t verified = 0;
        std::vector<std::byte> wavBytes;
        bool finished = false;
        const int channels = layout.channels;

        const auto take = [&]() {
            try {
                while (error.empty() && decoder.bufferAvailable()) {
                    const QAudioBuffer buffer = decoder.read();
                    if (!buffer.isValid()) {
                        continue;
                    }
                    const QAudioFormat format = buffer.format();
                    if (format.sampleRate() != layout.sampleRate || format.channelCount() != channels) {
                        error = "the encoded file decodes to another sample rate or channel count";
                        break;
                    }
                    const auto frames = static_cast<std::uint64_t>(buffer.frameCount());
                    if (verified + frames > layout.frames) {
                        error = "the encoded file holds more audio than the recording";
                        break;
                    }
                    wavBytes.resize(static_cast<std::size_t>(frames) * static_cast<std::size_t>(layout.blockAlign));
                    m_wav->readFrames(verified, wavBytes);
                    const auto samples = static_cast<qsizetype>(frames) * channels;
                    const std::optional<qsizetype> differs
                        = firstDifference(buffer, wavBytes.data(), samples, layout.bitsPerSample);
                    if (!differs) {
                        error = "the encoded file decodes to samples that cannot be compared at full precision";
                    } else if (*differs >= 0) {
                        const std::uint64_t frame = verified + static_cast<std::uint64_t>(*differs / channels);
                        error = "the encoded file differs from the recording at frame " + std::to_string(frame)
                            + ", channel " + std::to_string(*differs % channels + 1);
                    }
                    if (!error.empty()) {
                        break;
                    }
                    verified += frames;
                    waiter.alive();
                    // Short of 1 until the FLAC has its name: 1 is the
                    // end of the whole job.
                    report(std::min(LastCheckProgress, EncodingShare
                                                           + (1.0 - EncodingShare) * static_cast<double>(verified)
                                                               / static_cast<double>(layout.frames)));
                    if (m_options.cancel.cancelled()) {
                        waiter.cancelled = true;
                        break;
                    }
                }
            } catch (const std::exception &e) {
                error = std::string("the WAV could not be read again: ") + e.what();
            }
            if (!error.empty() || waiter.cancelled) {
                decoder.stop();
                waiter.finish();
            }
        };
        QObject::connect(&decoder, &QAudioDecoder::bufferReady, &waiter.loop, take);
        QObject::connect(&decoder, &QAudioDecoder::finished, &waiter.loop, [&]() {
            take();
            finished = true;
            waiter.finish();
        });
        QObject::connect(&decoder, QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error), &waiter.loop,
                         [&](QAudioDecoder::Error) {
                             if (error.empty()) {
                                 error = "the encoded file could not be decoded: " + decoder.errorString().toStdString();
                             }
                             waiter.finish();
                         });
        decoder.start();
        if (decoder.error() != QAudioDecoder::NoError && error.empty()) {
            error = "the encoded file could not be decoded: " + decoder.errorString().toStdString();
            waiter.finish();
        }
        waiter.run();
        decoder.stop();
        m_result.framesVerified = verified;

        if (waiter.cancelled) {
            m_cancelled = true;
            return false;
        }
        if (!error.empty()) {
            return fail(error);
        }
        if (waiter.stalled) {
            return fail("the check of the encoded file gave no sign of progress for "
                        + std::to_string(m_stallTimeoutMs) + " ms");
        }
        if (!finished || verified != layout.frames) {
            return fail("the encoded file holds " + std::to_string(verified) + " frames of the recording's "
                        + std::to_string(layout.frames));
        }
        // The WAV as it is now: one written to meanwhile is not the
        // recording that was compared.
        std::error_code ec;
        const fs::file_time_type modifiedNow = fs::last_write_time(longPathSafe(m_wavPath), ec);
        if (ec || modifiedNow != m_wavModified) {
            return fail("the WAV changed while it was encoded");
        }
        return true;
    }

    // Makes the proven file durable and gives it the target's name.
    bool place()
    {
        if (m_options.cancel.cancelled()) {
            m_cancelled = true;
            return false;
        }
        if (m_hooks.beforePlace) {
            m_hooks.beforePlace(pathToUtf8(m_temp));
        }
        // Opened without being created: a temporary file that went away
        // after the check is a failure, never an empty file under the
        // target's name.
        std::uint64_t flushedSize = 0;
        try {
            flushedSize = flushExistingFile(m_temp);
        } catch (const std::exception &e) {
            return fail(std::string("the encoded file could not be flushed: ") + e.what());
        }
        // The file that was flushed is the one that was checked: its size
        // and its STREAMINFO block, with the MD5 of every sample. Not its
        // audio again, unlike the tag writer, which re-hashes its copy:
        // decoding or hashing a FLAC of a whole set a second time costs
        // the person minutes on every keep, for a file changed in the
        // moment between the check and here by something else, which the
        // size and the MD5 already catch unless it rewrote the audio with
        // other bytes of the same length and left STREAMINFO alone.
        const std::optional<FlacStreamInfo> info = readFlacStreamInfo(m_temp);
        if (flushedSize != m_checkedSize || !info || info->raw != m_checkedInfo.raw) {
            return fail("the encoded file changed after it was checked");
        }
        // Asked again for a plain answer: the encoding takes minutes. The
        // rename below refuses a file that appears after this look too.
        const std::string targetName = pathToUtf8(m_target.filename());
        if (somethingAt(m_target) != std::optional<bool>(false)) {
            return fail(targetName + " appeared while the recording was encoded, and is not replaced");
        }
        if (m_hooks.beforeRename) {
            m_hooks.beforeRename(pathToUtf8(m_temp));
        }
        const NoReplaceRename renamed = renameNoReplace(m_temp, m_target);
        if (renamed.status == NoReplaceRename::Status::TargetExists) {
            return fail(targetName + " appeared while the recording was encoded, and is not replaced");
        }
        if (renamed.status != NoReplaceRename::Status::Renamed) {
            return fail("the encoded file could not be given its name: " + renamed.error);
        }
        m_tempMayExist = false;
        if (renamed.oldNameRemains) {
            // Given its name through a hard link whose old name could not
            // be removed: tried once more, and said when it stays.
            if (!removeSecondName(m_temp, m_target)) {
                m_result.notes.push_back(targetName + " also has a second name that could not be removed ("
                                         + renamed.error + "): " + pathToUtf8(m_temp)
                                         + ". It is the same file and can be deleted.");
            }
        }
        try {
            fsyncDirectoryContaining(pathToUtf8(m_target));
        } catch (...) {
        }
        // What has the name now is the size that was checked. Something
        // else is left where it is: it may be another program's.
        if (fileSizeOf(m_target) != std::optional<std::uint64_t>(m_checkedSize)) {
            return fail(targetName + " does not have the size of the checked file after it was given its name; "
                        "it was left as it is and does not count as kept");
        }
        return true;
    }

    void discardTemp()
    {
        if (!m_tempMayExist || sameFile(m_temp, m_wavPath) != std::optional<bool>(false)) {
            return;
        }
        std::error_code ec;
        if (fs::symlink_status(longPathSafe(m_temp), ec).type() == fs::file_type::regular) {
            fs::remove(longPathSafe(m_temp), ec);
        }
    }
};

}  // namespace

QtFlacRecordingEncoder::QtFlacRecordingEncoder(int stallTimeoutMs)
    : m_stallTimeoutMs(stallTimeoutMs)
{
}

bool QtFlacRecordingEncoder::encodingAvailable()
{
    if (QCoreApplication::instance() == nullptr) {
        return false;
    }
    QAudioDecoder decoder;
    return decoder.isSupported() && QMediaFormat(QMediaFormat::FLAC).isSupported(QMediaFormat::Encode);
}

application::RecordingEncodeResult QtFlacRecordingEncoder::encode(const std::string &wavPath, const std::string &flacPath,
                                                                  const application::RecordingEncodeOptions &options)
{
    try {
        // Absolute: the recorder takes a URL, and a relative one is nowhere.
        EncodeJob job(fs::absolute(pathFromUtf8(wavPath)), fs::absolute(pathFromUtf8(flacPath)), options,
                      m_stallTimeoutMs, m_hooks);
        return job.run();
    } catch (const std::exception &e) {
        application::RecordingEncodeResult result;
        result.error = std::string("the recording was not encoded: ") + e.what();
        return result;
    } catch (...) {
        application::RecordingEncodeResult result;
        result.error = "the recording was not encoded";
        return result;
    }
}

}  // namespace seabass::infrastructure::audio
