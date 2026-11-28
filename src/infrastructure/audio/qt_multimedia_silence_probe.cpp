// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/qt_multimedia_silence_probe.hpp"

#ifdef SEABASS_HAVE_FFMPEG_DECODER
#include "infrastructure/audio/analysis_pcm_source.hpp"
#include "infrastructure/audio/ffmpeg_audio_reader.hpp"
#endif

#include "infrastructure/audio/qt_decoder_start.hpp"

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <cmath>
#include <cstdint>
#include <limits>

namespace seabass::infrastructure::audio
{

namespace
{

// One decoded sample as a -1..1 amplitude, whatever the backend handed
// over. QAudioDecoder is asked for Float below, but a backend is free to
// ignore setAudioFormat(), and a silently misread integer buffer would
// read as either "all silence" or "never silent" -- both of which look
// like a working answer.
double sampleMagnitude(const QAudioBuffer &buffer, qsizetype frame, int channel)
{
    const QAudioFormat format = buffer.format();
    const int channels = format.channelCount();
    const qsizetype index = frame * channels + channel;

    switch (format.sampleFormat()) {
    case QAudioFormat::Float:
        return std::abs(static_cast<double>(buffer.constData<float>()[index]));
    case QAudioFormat::Int16:
        return std::abs(static_cast<double>(buffer.constData<qint16>()[index]))
            / static_cast<double>(std::numeric_limits<qint16>::max());
    case QAudioFormat::Int32:
        return std::abs(static_cast<double>(buffer.constData<qint32>()[index]))
            / static_cast<double>(std::numeric_limits<qint32>::max());
    case QAudioFormat::UInt8:
        // Unsigned, centred on 128.
        return std::abs(static_cast<double>(buffer.constData<quint8>()[index]) - 128.0) / 128.0;
    case QAudioFormat::Unknown:
    case QAudioFormat::NSampleFormats:
        break;
    }
    return -1.0;  // "cannot read this", distinct from a real 0.0
}

// The span of a file decoded end to end: how long it is, and how much
// of its start and its end stayed under the threshold.
domain::AudioContentSpan spanOf(qint64 framesSeen, qint64 firstLoudFrame, qint64 lastLoudFrame, int sampleRate)
{
    domain::AudioContentSpan span;
    span.totalSeconds = static_cast<double>(framesSeen) / sampleRate;
    if (firstLoudFrame < 0) {
        // Decoded end to end and never rose above the threshold. Silent
        // start to finish, reported as exactly that: total length, all
        // of it leading silence, no content. The caller refuses to match
        // on a zero-length content span, which is the right answer --
        // a file of pure silence is no more a copy of one recording than
        // of any other. (The library this project was built against had
        // 114 such files.)
        span.leadingSilenceSeconds = span.totalSeconds;
        span.trailingSilenceSeconds = 0.0;
        return span;
    }
    span.leadingSilenceSeconds = static_cast<double>(firstLoudFrame) / sampleRate;
    span.trailingSilenceSeconds = static_cast<double>(framesSeen - 1 - lastLoudFrame) / sampleRate;
    return span;
}

}  // namespace

bool QtMultimediaSilenceProbe::decodingAvailable()
{
#ifdef SEABASS_HAVE_FFMPEG_DECODER
    // The decoder on FFmpeg's libraries needs nothing of Qt Multimedia.
    if (ownDecoderInUse()) {
        return true;
    }
#endif
    if (QCoreApplication::instance() == nullptr) {
        return false;
    }
    QAudioDecoder decoder;
    return decoder.isSupported();
}

QtMultimediaSilenceProbe::QtMultimediaSilenceProbe(double silenceDb, int timeoutMs)
    : m_silenceThreshold(std::pow(10.0, silenceDb / 20.0)), m_timeoutMs(timeoutMs)
{
}

std::optional<domain::AudioContentSpan> QtMultimediaSilenceProbe::measureOnce(const std::string &absoluteFilePath,
                                                                              const decoder_start::Attempt &attempt,
                                                                              bool &gaveNothing)
{
    gaveNothing = false;
    if (absoluteFilePath.empty()) {
        return std::nullopt;
    }
    const QString path = QString::fromStdString(absoluteFilePath);
    // A stale catalog row pointing at a deleted file is ordinary on a
    // real stick, and costs a backend round trip to discover otherwise.
    if (!QFileInfo::exists(path)) {
        return std::nullopt;
    }
    if (QCoreApplication::instance() == nullptr) {
        // No event loop to run, so the decode would never progress.
        // Refusing here is far easier to diagnose than one timeout per
        // file.
        return std::nullopt;
    }

    QAudioDecoder decoder;
    if (!decoder.isSupported()) {
        // No decoding backend at all (a Qt built without the FFmpeg
        // plugin, a platform where it did not load). Answering nullopt
        // straight away beats one timeout per file.
        return std::nullopt;
    }
    QEventLoop loop;
    decoder_start::FirstSampleWatch watch(attempt, loop, m_timeoutMs);

    // Frame counts rather than timestamps. QAudioBuffer::startTime() is
    // not reliable across backends for a container whose first packet
    // carries encoder delay, and the arithmetic below only needs "how
    // many frames in", which the buffers themselves give exactly.
    qint64 framesSeen = 0;
    qint64 firstLoudFrame = -1;
    qint64 lastLoudFrame = -1;
    int sampleRate = 0;
    bool unreadableFormat = false;

    QObject::connect(&decoder, &QAudioDecoder::bufferReady, &loop, [&]() {
        if (!watch.heard()) {
            return;
        }
        while (decoder.bufferAvailable()) {
            const QAudioBuffer buffer = decoder.read();
            if (!buffer.isValid()) {
                continue;
            }
            const QAudioFormat format = buffer.format();
            const int channels = format.channelCount();
            if (channels <= 0 || format.sampleRate() <= 0) {
                continue;
            }
            sampleRate = format.sampleRate();
            const qsizetype frames = buffer.frameCount();
            for (qsizetype frame = 0; frame < frames; ++frame) {
                double peak = 0.0;
                for (int channel = 0; channel < channels; ++channel) {
                    const double magnitude = sampleMagnitude(buffer, frame, channel);
                    if (magnitude < 0.0) {
                        unreadableFormat = true;
                        loop.quit();
                        return;
                    }
                    if (magnitude > peak) {
                        peak = magnitude;
                    }
                }
                if (peak > m_silenceThreshold) {
                    if (firstLoudFrame < 0) {
                        firstLoudFrame = framesSeen + frame;
                    }
                    lastLoudFrame = framesSeen + frame;
                }
            }
            framesSeen += frames;
        }
    });

    // `done` rather than quitting the loop directly, because start()
    // below can emit either of these SYNCHRONOUSLY: the FFmpeg backend
    // calls error() straight out of start() when the container will not
    // open or carries no audio stream, and the connection is direct, so
    // the handler runs before exec() is ever entered -- where quit() is
    // documented to do nothing. The loop would then sit out the whole
    // timeout for a file the backend had already refused, which on a
    // stick with one corrupt track is 30 s of a scan spent waiting for
    // an answer already given.
    bool done = false;
    QObject::connect(&decoder, &QAudioDecoder::finished, &loop, [&]() {
        if (!watch.heard()) {
            return;
        }
        done = true;
        loop.quit();
    });
    // QOverload because the signal `error(Error)` shares its name with
    // the getter `error()`; without it the address is ambiguous.
    QObject::connect(&decoder, QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error), &loop,
                      [&](QAudioDecoder::Error) {
                          if (!watch.heard()) {
                              return;
                          }
                          done = true;
                          loop.quit();
                      });

    // Ask for the format the loop above is cheapest to read, and for
    // mono so a stereo file costs one comparison per frame rather than
    // two. A backend that ignores either is handled: sampleMagnitude()
    // reads whatever actually arrives, and the channel loop runs as many
    // times as the buffer says.
    QAudioFormat wanted;
    wanted.setSampleFormat(QAudioFormat::Float);
    wanted.setChannelCount(1);
    decoder.setAudioFormat(wanted);

    // A hard stop, so one damaged file cannot hang the scan that is
    // asking about it. A timeout leaves the partial counts below, which
    // is exactly why it does NOT answer with them -- see the guard on
    // `timedOut` after the loop.
    bool timedOut = false;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        loop.quit();
    });
    timeout.start(m_timeoutMs);

    decoder.setSource(QUrl::fromLocalFile(path));
    if (!attempt.silenced) {
        decoder.start();
    }
    // Only wait if there is still something to wait for -- see `done`.
    if (!done) {
        loop.exec();
    }
    decoder.stop();
    // A decoder that said nothing at all is not this file's answer:
    // measure() starts it again (qt_decoder_start.hpp).
    if (watch.gaveNothing()) {
        gaveNothing = true;
        return std::nullopt;
    }

    if (timedOut || unreadableFormat || decoder.error() != QAudioDecoder::NoError) {
        return std::nullopt;
    }
    if (sampleRate <= 0 || framesSeen <= 0) {
        return std::nullopt;
    }

    return spanOf(framesSeen, firstLoudFrame, lastLoudFrame, sampleRate);
}

#ifdef SEABASS_HAVE_FFMPEG_DECODER
// The same measure through the decoder on FFmpeg's libraries: the same
// frames, the same peak of the channels, the same counts as measureOnce()
// makes of Qt's buffers. One thread and no waiting, so the timeout is
// looked at between two frames.
std::optional<domain::AudioContentSpan> QtMultimediaSilenceProbe::measureThroughFfmpeg(const std::string &absoluteFilePath)
{
    // The hard stop, on a read that does not come back as well: the
    // reader asks this while it reads.
    QElapsedTimer taken;
    taken.start();
    const int timeoutMs = m_timeoutMs;
    FfmpegAudioReader reader(absoluteFilePath, [&taken, timeoutMs] { return taken.elapsed() >= timeoutMs; });
    if (!reader.error().empty()) {
        return std::nullopt;
    }
    qint64 framesSeen = 0;
    qint64 firstLoudFrame = -1;
    qint64 lastLoudFrame = -1;
    int sampleRate = 0;
    while (const std::optional<FfmpegAudioReader::Chunk> chunk = reader.next()) {
        if (taken.elapsed() >= m_timeoutMs) {
            return std::nullopt;
        }
        if (chunk->channels <= 0 || chunk->sampleRate <= 0) {
            continue;
        }
        sampleRate = chunk->sampleRate;
        const std::size_t channels = static_cast<std::size_t>(chunk->channels);
        for (std::size_t frame = 0; frame < chunk->frames; ++frame) {
            double peak = 0.0;
            for (std::size_t channel = 0; channel < channels; ++channel) {
                const double magnitude = std::abs(static_cast<double>(chunk->samples[frame * channels + channel]));
                if (magnitude > peak) {
                    peak = magnitude;
                }
            }
            if (peak > m_silenceThreshold) {
                if (firstLoudFrame < 0) {
                    firstLoudFrame = framesSeen + static_cast<qint64>(frame);
                }
                lastLoudFrame = framesSeen + static_cast<qint64>(frame);
            }
        }
        framesSeen += static_cast<qint64>(chunk->frames);
    }
    if (!reader.error().empty() || sampleRate <= 0 || framesSeen <= 0) {
        return std::nullopt;
    }
    return spanOf(framesSeen, firstLoudFrame, lastLoudFrame, sampleRate);
}
#endif

std::optional<domain::AudioContentSpan> QtMultimediaSilenceProbe::measure(const std::string &absoluteFilePath)
{
#ifdef SEABASS_HAVE_FFMPEG_DECODER
    if (ownDecoderInUse()) {
        if (absoluteFilePath.empty() || !QFileInfo::exists(QString::fromStdString(absoluteFilePath))) {
            return std::nullopt;
        }
        return measureThroughFfmpeg(absoluteFilePath);
    }
#endif
    std::optional<domain::AudioContentSpan> span;
    decoder_start::run(absoluteFilePath, [&](const decoder_start::Attempt &attempt) {
        bool gaveNothing = false;
        span = measureOnce(absoluteFilePath, attempt, gaveNothing);
        return gaveNothing;
    });
    return span;
}

}  // namespace seabass::infrastructure::audio
