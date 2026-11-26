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
#include "infrastructure/audio/wav_pcm_file.hpp"
#include "infrastructure/durable_file_write.hpp"
#include "infrastructure/long_paths.hpp"
#include "infrastructure/paths/utf8_path.hpp"
#include "infrastructure/stick_backup/posix_archive_file.hpp"

namespace seabass::infrastructure::audio
{

namespace
{

namespace fs = std::filesystem;

// Frames handed to the recorder at a time, and compared at a time in the
// check: a third of a second at 48 kHz, 128 KB of 24 bit stereo.
constexpr std::uint64_t FramesPerPiece = 16384;
// How much of the progress the encoding is; the check is the rest. The
// check decodes about as fast as the encoding runs.
constexpr double EncodingShare = 0.6;
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

// Whether something is under `path`, a dangling link included. nullopt
// when the filesystem would not say.
std::optional<bool> somethingAt(const fs::path &path)
{
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(longPathSafe(path), ec);
    if (status.type() == fs::file_type::not_found) {
        return false;
    }
    if (ec || status.type() == fs::file_type::none) {
        return std::nullopt;
    }
    return true;
}

class EncodeJob
{
public:
    EncodeJob(const fs::path &wavPath, const fs::path &target, const application::RecordingEncodeOptions &options,
              int stallTimeoutMs, std::function<void(const std::string &)> beforeCheck)
        : m_wavPath(wavPath)
        , m_target(target)
        , m_options(options)
        , m_stallTimeoutMs(stallTimeoutMs)
        , m_beforeCheck(std::move(beforeCheck))
    {
        m_temp = target;
        m_temp += RecordingEncodeTempSuffix;
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
        report(1.0);
        return m_result;
    }

private:
    fs::path m_wavPath;
    fs::path m_target;
    fs::path m_temp;
    std::string m_wavName;
    application::RecordingEncodeOptions m_options;
    int m_stallTimeoutMs;
    std::function<void(const std::string &)> m_beforeCheck;
    std::unique_ptr<WavPcmFile> m_wav;
    fs::file_time_type m_wavModified{};
    QByteArray m_wavMd5;
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
        m_wav = std::make_unique<WavPcmFile>(m_wavPath);
        if (!m_wav->isOpen()) {
            return refuse(m_wav->error());
        }
        const WavLayout &layout = m_wav->layout();
        m_result.sampleRate = layout.sampleRate;
        m_result.channels = layout.channels;
        m_result.bitsPerSample = layout.bitsPerSample;
        m_result.framesInWav = layout.frames;

        const std::string targetName = pathToUtf8(m_target.filename());
        const std::optional<bool> there = somethingAt(m_target);
        if (!there) {
            return refuse("the place for " + targetName + " could not be looked at");
        }
        if (*there) {
            return refuse(targetName + " exists already, and is not replaced");
        }
        const fs::path folder = m_target.parent_path();
        if (!fs::is_directory(longPathSafe(folder), ec)) {
            return refuse("the folder " + pathToUtf8(folder) + " cannot be written to: it is not a folder");
        }
        // A leftover of an earlier run goes, when it is a file; anything
        // else under that name is not this encoder's to take away.
        const fs::file_status leftover = fs::symlink_status(longPathSafe(m_temp), ec);
        if (leftover.type() == fs::file_type::regular || leftover.type() == fs::file_type::symlink) {
            fs::remove(longPathSafe(m_temp), ec);
        } else if (leftover.type() != fs::file_type::not_found) {
            return refuse("something is in the way of its temporary file " + pathToUtf8(m_temp.filename()));
        }
        if (somethingAt(m_temp) != std::optional<bool>(false)) {
            return refuse("its temporary file " + pathToUtf8(m_temp.filename()) + " could not be cleared");
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
        if (!actual.isEmpty() && pathFromUtf8(actual.toStdString()) != m_temp) {
            // The recorder wrote somewhere else; that file is its, and
            // goes as well.
            std::error_code ec;
            fs::remove(longPathSafe(pathFromUtf8(actual.toStdString())), ec);
            return fail("the encoder wrote to " + actual.toStdString() + " and not where it was asked to");
        }
        m_wavMd5 = md5.result();
        return true;
    }

    // Decodes the temporary file and compares it with the WAV, sample for
    // sample, every channel, and the number of frames.
    bool verifyTemp()
    {
        if (m_beforeCheck) {
            m_beforeCheck(pathToUtf8(m_temp));
        }
        const WavLayout &layout = m_wav->layout();
        const std::optional<FlacStreamInfo> info = readFlacStreamInfo(m_temp);
        if (!info) {
            return fail("what the encoder wrote is not a FLAC file");
        }
        if (info->sampleRate != layout.sampleRate || info->channels != layout.channels
            || info->bitsPerSample != layout.bitsPerSample) {
            return fail("the encoder wrote " + std::to_string(info->bitsPerSample) + " bit, "
                        + std::to_string(info->channels) + " channel audio at " + std::to_string(info->sampleRate)
                        + " Hz, not the recording's " + std::to_string(layout.bitsPerSample) + " bit, "
                        + std::to_string(layout.channels) + " channel audio at " + std::to_string(layout.sampleRate)
                        + " Hz");
        }
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
                    report(EncodingShare
                           + (1.0 - EncodingShare) * static_cast<double>(verified) / static_cast<double>(layout.frames));
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
        try {
            stick_backup::PosixArchiveFile written(longPathSafe(m_temp), stick_backup::PosixArchiveFile::OpenMode::ReadWrite);
            written.barrier();
        } catch (const std::exception &e) {
            return fail(std::string("the encoded file could not be flushed: ") + e.what());
        }
        // Asked again: the encoding takes minutes. The rename itself would
        // replace.
        const std::optional<bool> there = somethingAt(m_target);
        if (there != std::optional<bool>(false)) {
            return fail(pathToUtf8(m_target.filename()) + " appeared while the recording was encoded, and is not replaced");
        }
        std::error_code ec;
        fs::rename(longPathSafe(m_temp), longPathSafe(m_target), ec);
        if (ec) {
            return fail("the encoded file could not be given its name: " + ec.message());
        }
        m_tempMayExist = false;
        try {
            fsyncDirectoryContaining(pathToUtf8(m_target));
        } catch (...) {
        }
        return true;
    }

    void discardTemp()
    {
        if (!m_tempMayExist) {
            return;
        }
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(longPathSafe(m_temp), ec);
        if (status.type() == fs::file_type::regular || status.type() == fs::file_type::symlink) {
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
                      m_stallTimeoutMs, m_beforeCheck);
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
