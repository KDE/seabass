// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/qt_multimedia_detail_waveform.hpp"

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <optional>
#include <vector>

#include "domain/audio_detail_waveform.hpp"

namespace seabass::infrastructure::audio
{

namespace
{

// One buffer as mono floats, whatever sample format the backend chose;
// false for one this cannot read.
bool toMono(const QAudioBuffer &buffer, std::vector<float> &mono)
{
    const QAudioFormat format = buffer.format();
    const int channels = format.channelCount();
    const qsizetype frames = buffer.frameCount();
    mono.assign(static_cast<std::size_t>(frames), 0.0F);
    for (qsizetype frame = 0; frame < frames; ++frame) {
        float sum = 0.0F;
        for (int channel = 0; channel < channels; ++channel) {
            const qsizetype index = frame * channels + channel;
            switch (format.sampleFormat()) {
            case QAudioFormat::Float:
                sum += buffer.constData<float>()[index];
                break;
            case QAudioFormat::Int16:
                sum += static_cast<float>(buffer.constData<qint16>()[index]) / 32768.0F;
                break;
            case QAudioFormat::Int32:
                sum += static_cast<float>(static_cast<double>(buffer.constData<qint32>()[index]) / 2147483648.0);
                break;
            case QAudioFormat::UInt8:
                sum += (static_cast<float>(buffer.constData<quint8>()[index]) - 128.0F) / 128.0F;
                break;
            case QAudioFormat::Unknown:
            case QAudioFormat::NSampleFormats:
                return false;
            }
        }
        mono[static_cast<std::size_t>(frame)] = sum / static_cast<float>(channels);
    }
    return true;
}

}  // namespace

bool decodeMono(const std::string &absoluteFilePath,
                const std::function<void(const float *samples, std::size_t count, int sampleRate)> &sink,
                const std::function<bool()> &cancelled, int timeoutMs,
                const std::function<void(long long durationMs)> &durationKnown)
{
    if (absoluteFilePath.empty() || QCoreApplication::instance() == nullptr) {
        return false;
    }
    const QString path = QString::fromStdString(absoluteFilePath);
    if (!QFileInfo::exists(path)) {
        return false;
    }
    QAudioDecoder decoder;
    if (!decoder.isSupported()) {
        return false;
    }
    QEventLoop loop;
    int sampleRate = 0;
    bool failed = false;
    bool done = false;
    std::vector<float> mono;

    // Frames counted from the first buffer the decoder hands over, as
    // qt_decoder_timeline_test measures the timeline by.
    QObject::connect(&decoder, &QAudioDecoder::bufferReady, &loop, [&]() {
        while (decoder.bufferAvailable()) {
            const QAudioBuffer buffer = decoder.read();
            if (failed || !buffer.isValid()) {
                continue;
            }
            if (cancelled && cancelled()) {
                failed = true;
                loop.quit();
                return;
            }
            const QAudioFormat format = buffer.format();
            if (format.channelCount() <= 0 || format.sampleRate() <= 0) {
                continue;
            }
            if (sampleRate == 0) {
                sampleRate = format.sampleRate();
            } else if (format.sampleRate() != sampleRate) {
                failed = true;
                loop.quit();
                return;
            }
            if (!toMono(buffer, mono)) {
                failed = true;
                loop.quit();
                return;
            }
            sink(mono.data(), mono.size(), sampleRate);
        }
    });
    // start() can emit either of these synchronously; see
    // QtMultimediaSilenceProbe::measure().
    QObject::connect(&decoder, &QAudioDecoder::finished, &loop, [&]() {
        done = true;
        loop.quit();
    });
    QObject::connect(&decoder, QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error), &loop,
                     [&](QAudioDecoder::Error) {
                         failed = true;
                         done = true;
                         loop.quit();
                     });
    if (durationKnown) {
        QObject::connect(&decoder, &QAudioDecoder::durationChanged, &loop, [&](qint64 ms) {
            if (ms > 0) {
                durationKnown(ms);
            }
        });
    }
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        failed = true;
        loop.quit();
    });
    timeout.start(timeoutMs);

    // Float and mono asked for, without a rate. A format without a rate
    // is not a valid QAudioFormat, and the FFmpeg backend then keeps the
    // file's own rate and channels (in whatever sample format it picks).
    // So nothing is resampled and nothing moves in time; toMono() reads
    // whatever comes and averages the channels.
    QAudioFormat wanted;
    wanted.setSampleFormat(QAudioFormat::Float);
    wanted.setChannelCount(1);
    decoder.setAudioFormat(wanted);
    decoder.setSource(QUrl::fromLocalFile(path));
    decoder.start();
    if (!done && !failed) {
        loop.exec();
    }
    decoder.stop();
    return done && !failed && sampleRate > 0 && decoder.error() == QAudioDecoder::NoError;
}

domain::DetailWaveform audioDetailWaveform(const std::string &absoluteFilePath, const std::function<bool()> &cancelled)
{
    std::optional<domain::AudioDetailWaveformBuilder> builder;
    long long durationMs = 0;
    // Room for every column at once, once both the rate and the length
    // are known: a two-hour mix's columns are not grown by doubling.
    const auto reserve = [&]() {
        if (builder && durationMs > 0) {
            builder->reserveSeconds(static_cast<double>(durationMs) / 1000.0 + 1.0);
        }
    };
    const bool decoded = decodeMono(
        absoluteFilePath,
        [&](const float *samples, std::size_t count, int sampleRate) {
            if (!builder) {
                builder.emplace(sampleRate);
                reserve();
            }
            builder->push(samples, count);
        },
        cancelled, 120000,
        [&](long long ms) {
            durationMs = ms;
            reserve();
        });
    if (!decoded || !builder) {
        return {};
    }
    return builder->finish();
}

}  // namespace seabass::infrastructure::audio
