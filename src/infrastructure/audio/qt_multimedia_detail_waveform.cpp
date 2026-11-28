// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/qt_multimedia_detail_waveform.hpp"

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

// One start of the decoder; `gaveNothing` where it said nothing at all
// before its first sample, which decodeMono() starts again.
bool decodeMonoOnce(const std::string &absoluteFilePath,
                    const std::function<void(const float *samples, std::size_t count, int sampleRate)> &sink,
                    const std::function<bool()> &cancelled, int timeoutMs,
                    const std::function<void(long long durationMs)> &durationKnown, const decoder_start::Attempt &attempt,
                    bool &gaveNothing)
{
    gaveNothing = false;
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
    decoder_start::FirstSampleWatch watch(attempt, loop, timeoutMs);
    int sampleRate = 0;
    bool failed = false;
    bool done = false;
    std::vector<float> mono;

    // Frames counted from the first buffer the decoder hands over, as
    // qt_decoder_timeline_test measures the timeline by.
    QObject::connect(&decoder, &QAudioDecoder::bufferReady, &loop, [&]() {
        if (!watch.heard()) {
            return;
        }
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
        if (!watch.heard()) {
            return;
        }
        done = true;
        loop.quit();
    });
    QObject::connect(&decoder, QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error), &loop,
                     [&](QAudioDecoder::Error) {
                         if (!watch.heard()) {
                             return;
                         }
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
    if (!attempt.silenced) {
        decoder.start();
    }
    if (!done && !failed) {
        loop.exec();
    }
    decoder.stop();
    // A decoder that said nothing at all is not this file's answer (and
    // the sink has had nothing): decodeMono() starts it again.
    if (watch.gaveNothing()) {
        gaveNothing = true;
        return false;
    }
    return done && !failed && sampleRate > 0 && decoder.error() == QAudioDecoder::NoError;
}

#ifdef SEABASS_HAVE_FFMPEG_DECODER
// The same through the decoder on FFmpeg's libraries: the channels of
// every frame added up and divided as toMono() does it for Qt's float
// buffers, in floats. One thread and no waiting, so the cancel and the
// timeout are looked at between two frames.
bool decodeMonoThroughFfmpeg(const std::string &absoluteFilePath,
                             const std::function<void(const float *samples, std::size_t count, int sampleRate)> &sink,
                             const std::function<bool()> &cancelled, int timeoutMs,
                             const std::function<void(long long durationMs)> &durationKnown)
{
    if (absoluteFilePath.empty() || !QFileInfo::exists(QString::fromStdString(absoluteFilePath))) {
        return false;
    }
    // The cancel and the timeout, on a read that does not come back as
    // well: the reader asks this while it reads.
    QElapsedTimer taken;
    taken.start();
    const auto stop = [&] { return (cancelled && cancelled()) || taken.elapsed() >= timeoutMs; };
    FfmpegAudioReader reader(absoluteFilePath, stop);
    if (!reader.error().empty()) {
        return false;
    }
    if (durationKnown && reader.durationMs() > 0) {
        durationKnown(reader.durationMs());
    }
    int sampleRate = 0;
    std::vector<float> mono;
    while (const std::optional<FfmpegAudioReader::Chunk> chunk = reader.next()) {
        if (stop()) {
            return false;
        }
        if (chunk->channels <= 0 || chunk->sampleRate <= 0) {
            continue;
        }
        if (sampleRate == 0) {
            sampleRate = chunk->sampleRate;
        } else if (chunk->sampleRate != sampleRate) {
            return false;
        }
        const std::size_t channels = static_cast<std::size_t>(chunk->channels);
        mono.assign(chunk->frames, 0.0F);
        for (std::size_t frame = 0; frame < chunk->frames; ++frame) {
            float sum = 0.0F;
            for (std::size_t channel = 0; channel < channels; ++channel) {
                sum += chunk->samples[frame * channels + channel];
            }
            mono[frame] = sum / static_cast<float>(chunk->channels);
        }
        sink(mono.data(), mono.size(), sampleRate);
    }
    return reader.error().empty() && sampleRate > 0;
}
#endif

}  // namespace

bool decodeMono(const std::string &absoluteFilePath,
                const std::function<void(const float *samples, std::size_t count, int sampleRate)> &sink,
                const std::function<bool()> &cancelled, int timeoutMs,
                const std::function<void(long long durationMs)> &durationKnown)
{
#ifdef SEABASS_HAVE_FFMPEG_DECODER
    if (ownDecoderInUse()) {
        return decodeMonoThroughFfmpeg(absoluteFilePath, sink, cancelled, timeoutMs, durationKnown);
    }
#endif
    bool decoded = false;
    decoder_start::run(absoluteFilePath, [&](const decoder_start::Attempt &attempt) {
        bool gaveNothing = false;
        decoded = decodeMonoOnce(absoluteFilePath, sink, cancelled, timeoutMs, durationKnown, attempt, gaveNothing);
        // A cancel while it waited is not a reason to start again.
        return gaveNothing && !(cancelled && cancelled());
    });
    return decoded;
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
