// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/qt_multimedia_silence_probe.hpp"

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QCoreApplication>
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

}  // namespace

QtMultimediaSilenceProbe::QtMultimediaSilenceProbe(double silenceDb, int timeoutMs)
    : m_silenceThreshold(std::pow(10.0, silenceDb / 20.0)), m_timeoutMs(timeoutMs)
{
}

std::optional<domain::AudioContentSpan> QtMultimediaSilenceProbe::measure(const std::string &absoluteFilePath)
{
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

    QObject::connect(&decoder, &QAudioDecoder::finished, &loop, &QEventLoop::quit);
    // QOverload because the signal `error(Error)` shares its name with
    // the getter `error()`; without it the address is ambiguous.
    QObject::connect(&decoder, QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error), &loop,
                      [&](QAudioDecoder::Error) { loop.quit(); });

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
    decoder.start();
    loop.exec();
    decoder.stop();

    if (timedOut || unreadableFormat || decoder.error() != QAudioDecoder::NoError) {
        return std::nullopt;
    }
    if (sampleRate <= 0 || framesSeen <= 0) {
        return std::nullopt;
    }

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

}  // namespace seabass::infrastructure::audio
