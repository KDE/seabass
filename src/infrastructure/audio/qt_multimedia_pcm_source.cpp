// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/qt_multimedia_pcm_source.hpp"

#include "infrastructure/audio/qt_decoder_start.hpp"

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
#include <memory>
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
    // Made anew for every attempt at opening the file (see open()).
    std::unique_ptr<QAudioDecoder> made;
    int stallTimeoutMs = 10000;
    // How long this wait may last: stallTimeoutMs, but for the first
    // sample of an attempt that another follows.
    int waitMs = 10000;
    int attempts = 0;
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
        while (made->bufferAvailable()) {
            const QAudioBuffer buffer = made->read();
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
    // The error is the stall timer's: nothing came, and the decoder
    // itself reported no fault.
    bool gaveNothing = false;

    void answer()
    {
        answered = true;
        if (loop != nullptr) {
            loop->quit();
        }
    }

    void connectDecoder()
    {
        QObject::connect(made.get(), &QAudioDecoder::bufferReady, made.get(), [this]() {
            if (!takeBuffers()) {
                made->stop();
            }
            answer();
        });
        QObject::connect(made.get(), &QAudioDecoder::finished, made.get(), [this]() {
            if (takeBuffers()) {
                finished = true;
            }
            answer();
        });
        QObject::connect(made.get(), QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error), made.get(),
                         [this](QAudioDecoder::Error) {
                             if (error.empty()) {
                                 error = "decoding failed: " + made->errorString().toStdString();
                             }
                             answer();
                         });
    }

    // One attempt at the file: a decoder of its own, started, and waited
    // for up to its first sample. `mute` (a test's) leaves the decoder
    // unstarted, which is what the fault looks like from here: no
    // buffer, no end, no error.
    void open(const QString &path, bool mute)
    {
        if (made) {
            made->stop();
        }
        made = std::make_unique<QAudioDecoder>();
        error.clear();
        gaveNothing = false;
        finished = false;
        sampleRate = 0;
        samples.clear();
        position = 0;
        peakHeld = 0;
        // Mono floats are what the port hands on; asked for here so that
        // the backend mixes down. A backend that ignores it is handled by
        // takeBuffers(), which mixes and converts whatever arrives.
        QAudioFormat wanted;
        wanted.setSampleFormat(QAudioFormat::Float);
        wanted.setChannelCount(1);
        made->setAudioFormat(wanted);
        made->setSource(QUrl::fromLocalFile(path));
        connectDecoder();
        if (!mute) {
            made->start();
        }
        // A backend that refuses the file may say so from start() itself.
        if (error.empty() && made->error() != QAudioDecoder::NoError) {
            error = "decoding failed: " + made->errorString().toStdString();
        }
        while (sampleRate == 0 && !finished && error.empty()) {
            waitForMore();
        }
        if (sampleRate == 0 && error.empty()) {
            error = "the decoder gave no audio";
        }
    }

    // Runs the event loop until a buffer has come, the decoder is done or
    // has failed, or nothing came for waitMs.
    void waitForMore()
    {
        if (finished || !error.empty()) {
            return;
        }
        answered = false;
        if (made->bufferAvailable()) {
            if (!takeBuffers()) {
                made->stop();
            }
            return;
        }
        QEventLoop events;
        QTimer stall;
        stall.setSingleShot(true);
        QObject::connect(&stall, &QTimer::timeout, &events, [this]() {
            if (error.empty()) {
                error = "the decoder gave nothing for " + std::to_string(waitMs) + " ms";
                gaveNothing = true;
            }
            answer();
        });
        stall.start(waitMs);
        loop = &events;
        if (!answered) {
            events.exec();
        }
        loop = nullptr;
        if (!error.empty()) {
            made->stop();
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
    if (!QAudioDecoder().isSupported()) {
        d->error = "no audio decoding backend (QAudioDecoder::isSupported() is false)";
        return;
    }
    // Started again where the decoder gave nothing at all before its
    // first sample (qt_decoder_start.hpp says what that is and what was
    // measured). An attempt before the last waits a few seconds for the
    // first sample, the last one the whole stall timeout.
    d->attempts = decoder_start::run(absoluteFilePath, [&](const decoder_start::Attempt &attempt) {
        d->waitMs = attempt.firstSampleWaitMs > 0 ? std::min(attempt.firstSampleWaitMs, d->stallTimeoutMs)
                                                  : d->stallTimeoutMs;
        d->open(path, attempt.silenced);
        // Whatever came after the wait ran out belongs to an attempt
        // that is over: nothing has been handed on yet, and open()
        // begins the next one from nothing.
        return d->gaveNothing;
    });
    d->waitMs = d->stallTimeoutMs;
}

int QtMultimediaPcmSource::openAttempts() const
{
    return d->attempts;
}

QtMultimediaPcmSource::~QtMultimediaPcmSource()
{
    if (d && d->made) {
        d->made->stop();
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
