// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/qt_multimedia_pcm_source.hpp"

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

namespace seabass::infrastructure::audio
{

namespace
{

// One decoded sample as a -1..1 value, whatever the backend handed over.
// Float is asked for, but a backend is free to ignore setAudioFormat(),
// and an integer buffer read as floats would be noise that looks like
// audio. nullopt for a format this cannot read.
std::optional<float> sampleValue(const QAudioBuffer &buffer, qsizetype index)
{
    switch (buffer.format().sampleFormat()) {
    case QAudioFormat::Float:
        return buffer.constData<float>()[index];
    case QAudioFormat::Int16:
        return static_cast<float>(buffer.constData<qint16>()[index] / 32768.0);
    case QAudioFormat::Int32:
        return static_cast<float>(buffer.constData<qint32>()[index] / 2147483648.0);
    case QAudioFormat::UInt8:
        return static_cast<float>((buffer.constData<quint8>()[index] - 128.0) / 128.0);
    case QAudioFormat::Unknown:
    case QAudioFormat::NSampleFormats:
        break;
    }
    return std::nullopt;
}

}  // namespace

struct QtMultimediaPcmSource::State
{
    QAudioDecoder decoder;
    int stallTimeoutMs = 10000;
    int sampleRate = 0;
    std::string error;
    // Decoded and not yet read: samples[position..].
    std::vector<float> samples;
    std::size_t position = 0;
    std::size_t peakHeld = 0;
    bool finished = false;

    // Takes every buffer the decoder has ready. False when one cannot be
    // read, with `error` set.
    bool takeBuffers()
    {
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
            if (sampleRate == 0) {
                sampleRate = format.sampleRate();
            } else if (format.sampleRate() != sampleRate) {
                error = "the decoder changed the sample rate part of the way through";
                return false;
            }
            // Drop what has been read before adding, so the vector holds
            // only what waits.
            if (position > 0) {
                samples.erase(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(position));
                position = 0;
            }
            const qsizetype frames = buffer.frameCount();
            samples.reserve(samples.size() + static_cast<std::size_t>(frames));
            // The mono mix the analyser hears: a change here raises
            // AnalysisInputRevision (the header).
            for (qsizetype frame = 0; frame < frames; ++frame) {
                double sum = 0.0;
                for (int channel = 0; channel < channels; ++channel) {
                    const std::optional<float> value = sampleValue(buffer, frame * channels + channel);
                    if (!value) {
                        error = "the decoder gave samples in a format this cannot read";
                        return false;
                    }
                    sum += *value;
                }
                samples.push_back(static_cast<float>(sum / channels));
            }
            peakHeld = std::max(peakHeld, samples.size() - position);
        }
        return true;
    }

    // The loop waitForMore() runs, while it runs; and whether the decoder
    // answered since it was entered. The decoder's signals may come
    // straight out of start() or read(), before exec() is entered, where
    // quit() does nothing; `answered` then keeps the loop from being
    // entered at all.
    QEventLoop *loop = nullptr;
    bool answered = false;

    void answer()
    {
        answered = true;
        if (loop != nullptr) {
            loop->quit();
        }
    }

    void connectDecoder()
    {
        QObject::connect(&decoder, &QAudioDecoder::bufferReady, &decoder, [this]() {
            if (!takeBuffers()) {
                decoder.stop();
            }
            answer();
        });
        QObject::connect(&decoder, &QAudioDecoder::finished, &decoder, [this]() {
            if (takeBuffers()) {
                finished = true;
            }
            answer();
        });
        QObject::connect(&decoder, QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error), &decoder,
                         [this](QAudioDecoder::Error) {
                             if (error.empty()) {
                                 error = "decoding failed: " + decoder.errorString().toStdString();
                             }
                             answer();
                         });
    }

    // Runs the event loop until a buffer has come, the decoder is done or
    // has failed, or nothing came for stallTimeoutMs.
    void waitForMore()
    {
        if (finished || !error.empty()) {
            return;
        }
        answered = false;
        if (decoder.bufferAvailable()) {
            if (!takeBuffers()) {
                decoder.stop();
            }
            return;
        }
        QEventLoop events;
        QTimer stall;
        stall.setSingleShot(true);
        QObject::connect(&stall, &QTimer::timeout, &events, [this]() {
            if (error.empty()) {
                error = "the decoder gave nothing for " + std::to_string(stallTimeoutMs) + " ms";
            }
            answer();
        });
        stall.start(stallTimeoutMs);
        loop = &events;
        if (!answered) {
            events.exec();
        }
        loop = nullptr;
        if (!error.empty()) {
            decoder.stop();
        }
    }
};

bool QtMultimediaPcmSource::decodingAvailable()
{
    if (QCoreApplication::instance() == nullptr) {
        return false;
    }
    QAudioDecoder decoder;
    return decoder.isSupported();
}

QtMultimediaPcmSource::QtMultimediaPcmSource(const std::string &absoluteFilePath, int stallTimeoutMs)
    : d(std::make_unique<State>())
{
    d->stallTimeoutMs = stallTimeoutMs;
    const QString path = QString::fromStdString(absoluteFilePath);
    if (absoluteFilePath.empty() || !QFileInfo::exists(path)) {
        d->error = "no such file: " + absoluteFilePath;
        return;
    }
    if (QCoreApplication::instance() == nullptr) {
        // No event loop to run, so the decode would never progress.
        d->error = "no QCoreApplication: the decoder needs an event loop";
        return;
    }
    if (!d->decoder.isSupported()) {
        d->error = "no audio decoding backend (QAudioDecoder::isSupported() is false)";
        return;
    }
    // Mono floats are what the port hands on; asked for here so that the
    // backend mixes down. A backend that ignores it is handled by
    // takeBuffers(), which mixes and converts whatever arrives.
    QAudioFormat wanted;
    wanted.setSampleFormat(QAudioFormat::Float);
    wanted.setChannelCount(1);
    d->decoder.setAudioFormat(wanted);
    d->decoder.setSource(QUrl::fromLocalFile(path));
    d->connectDecoder();
    d->decoder.start();
    // A backend that refuses the file may say so from start() itself.
    if (d->error.empty() && d->decoder.error() != QAudioDecoder::NoError) {
        d->error = "decoding failed: " + d->decoder.errorString().toStdString();
    }
    while (d->sampleRate == 0 && !d->finished && d->error.empty()) {
        d->waitForMore();
    }
    if (d->sampleRate == 0 && d->error.empty()) {
        d->error = "the decoder gave no audio";
    }
}

QtMultimediaPcmSource::~QtMultimediaPcmSource()
{
    if (d) {
        d->decoder.stop();
    }
}

int QtMultimediaPcmSource::sampleRate() const
{
    return d->sampleRate;
}

std::size_t QtMultimediaPcmSource::read(std::span<float> out)
{
    std::size_t written = 0;
    while (written < out.size()) {
        if (d->position >= d->samples.size()) {
            d->samples.clear();
            d->position = 0;
            if (d->finished || !d->error.empty()) {
                break;
            }
            d->waitForMore();
            continue;
        }
        const std::size_t count = std::min(out.size() - written, d->samples.size() - d->position);
        std::copy_n(d->samples.begin() + static_cast<std::ptrdiff_t>(d->position), count,
                    out.begin() + static_cast<std::ptrdiff_t>(written));
        d->position += count;
        written += count;
    }
    return written;
}

std::string QtMultimediaPcmSource::error() const
{
    return d->error;
}

std::size_t QtMultimediaPcmSource::peakHeldSamples() const
{
    return d->peakHeld;
}

}  // namespace seabass::infrastructure::audio
