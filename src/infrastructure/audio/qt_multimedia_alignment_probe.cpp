// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/audio/qt_multimedia_alignment_probe.hpp"

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

#include <cmath>
#include <limits>

#include "domain/audio_alignment.hpp"
#include "domain/matching_policy.hpp"
#include "infrastructure/audio/qt_multimedia_silence_probe.hpp"

namespace seabass::infrastructure::audio
{

namespace
{

// One decoded sample as a -1..1 value, whatever the backend handed over;
// NaN for a sample format this cannot read. Signed, unlike the silence
// probe's magnitude: a correlation needs the waveform, not its size.
double sampleValue(const QAudioBuffer &buffer, qsizetype frame, int channel)
{
    const QAudioFormat format = buffer.format();
    const qsizetype index = frame * format.channelCount() + channel;
    switch (format.sampleFormat()) {
    case QAudioFormat::Float:
        return static_cast<double>(buffer.constData<float>()[index]);
    case QAudioFormat::Int16:
        return static_cast<double>(buffer.constData<qint16>()[index]) / 32768.0;
    case QAudioFormat::Int32:
        return static_cast<double>(buffer.constData<qint32>()[index]) / 2147483648.0;
    case QAudioFormat::UInt8:
        return (static_cast<double>(buffer.constData<quint8>()[index]) - 128.0) / 128.0;
    case QAudioFormat::Unknown:
    case QAudioFormat::NSampleFormats:
        break;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

}  // namespace

QtMultimediaAlignmentProbe::QtMultimediaAlignmentProbe(int timeoutMs, std::size_t cachedBytes)
    : m_timeoutMs(timeoutMs), m_cachedBytes(cachedBytes)
{
}

std::size_t QtMultimediaAlignmentProbe::cachedBytes() const
{
    std::size_t bytes = 0;
    for (const Entry &entry : m_cache) {
        bytes += entry.samples ? entry.samples->bytes() : 0;
    }
    return bytes;
}

bool QtMultimediaAlignmentProbe::decodingAvailable()
{
    return QtMultimediaSilenceProbe::decodingAvailable();
}

std::shared_ptr<const domain::alignment::KeptSamples> QtMultimediaAlignmentProbe::decoded(
    const std::string &absoluteFilePath)
{
    for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
        if (it->path == absoluteFilePath) {
            m_cache.splice(m_cache.begin(), m_cache, it);
            return m_cache.front().samples;
        }
    }
    auto samples = decode(absoluteFilePath);
    m_cache.push_front({absoluteFilePath, samples});
    // The least recently used go first; the one just decoded stays
    // whatever it weighs, since the pair being measured needs it.
    while (m_cache.size() > 1 && cachedBytes() > m_cachedBytes) {
        m_cache.pop_back();
    }
    return samples;
}

domain::AudioAlignmentProbe::Answer QtMultimediaAlignmentProbe::align(const std::string &absolutePathA,
                                                                      const std::string &absolutePathB)
{
    const auto a = decoded(absolutePathA);
    if (!a) {
        return Answer::noOpinion(true, false);
    }
    const auto b = decoded(absolutePathB);
    if (!b) {
        return Answer::noOpinion(false, true);
    }
    // Copies further apart in length than the middle kept of each can
    // reach are not measured: not "does not line up", which a cache
    // would keep, but nothing said.
    if (!domain::alignment::alignmentCovers(*a, *b, domain::alignment::AnalysisRate)) {
        return Answer::noOpinion();
    }
    const std::optional<double> shift = domain::alignment::measureShiftMs(
        *a, *b, domain::alignment::AnalysisRate,
        domain::alignment::shiftSearchSeconds(domain::MatchingPolicy::compareAudioSeconds()));
    return shift ? Answer::linedUp(*shift) : Answer::notLinedUp();
}

std::shared_ptr<const domain::alignment::KeptSamples> QtMultimediaAlignmentProbe::decodeOnce(
    const std::string &absoluteFilePath, const decoder_start::Attempt &attempt, bool &gaveNothing) const
{
    gaveNothing = false;
    if (absoluteFilePath.empty()) {
        return nullptr;
    }
    const QString path = QString::fromStdString(absoluteFilePath);
    if (!QFileInfo::exists(path) || QCoreApplication::instance() == nullptr) {
        return nullptr;
    }
    QAudioDecoder decoder;
    if (!decoder.isSupported()) {
        return nullptr;
    }
    QEventLoop loop;
    decoder_start::FirstSampleWatch watch(attempt, loop, m_timeoutMs);

    // Frames are counted from the first buffer, not read off
    // QAudioBuffer::startTime(): the silence probe's reasoning, and the
    // one qt_decoder_timeline_test measures the decoder's timeline by.
    std::optional<domain::alignment::Resampler> resampler;
    domain::alignment::KeptSamplesBuilder kept(domain::alignment::AnalysisRate);
    int sampleRate = 0;
    bool unreadable = false;

    QObject::connect(&decoder, &QAudioDecoder::bufferReady, &loop, [&]() {
        if (!watch.heard()) {
            return;
        }
        while (decoder.bufferAvailable()) {
            const QAudioBuffer buffer = decoder.read();
            if (!buffer.isValid() || unreadable) {
                continue;
            }
            const QAudioFormat format = buffer.format();
            const int channels = format.channelCount();
            if (channels <= 0 || format.sampleRate() <= 0) {
                continue;
            }
            if (sampleRate == 0) {
                sampleRate = format.sampleRate();
                resampler.emplace(static_cast<double>(sampleRate), domain::alignment::AnalysisRate);
            } else if (format.sampleRate() != sampleRate) {
                // A stream that changes rate partway has no one timeline
                // to count frames on.
                unreadable = true;
                loop.quit();
                return;
            }
            const qsizetype frames = buffer.frameCount();
            for (qsizetype frame = 0; frame < frames; ++frame) {
                double sum = 0.0;
                for (int channel = 0; channel < channels; ++channel) {
                    const double value = sampleValue(buffer, frame, channel);
                    if (std::isnan(value)) {
                        unreadable = true;
                        loop.quit();
                        return;
                    }
                    sum += value;
                }
                resampler->push(static_cast<float>(sum / channels));
            }
            // Handed on buffer by buffer, so what no window can need is
            // let go of while the rest is still decoding.
            const std::vector<float> ready = resampler->drain();
            kept.append(ready);
        }
    });

    // See QtMultimediaSilenceProbe::measure() for why `done` and not a
    // bare quit(): start() can emit either of these synchronously.
    bool done = false;
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
                         done = true;
                         loop.quit();
                     });

    // Float and mono asked for, as the silence probe asks; a backend that
    // ignores either is read as it comes (sampleValue(), and the channels
    // averaged above). The rate is left to the file: the resampler brings
    // it to the analysis rate without moving anything in time, which a
    // backend's own resampler is not known to promise.
    QAudioFormat wanted;
    wanted.setSampleFormat(QAudioFormat::Float);
    wanted.setChannelCount(1);
    decoder.setAudioFormat(wanted);

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
    if (!done) {
        loop.exec();
    }
    decoder.stop();
    // A decoder that said nothing at all is not this file's answer:
    // decode() starts it again (qt_decoder_start.hpp).
    if (watch.gaveNothing()) {
        gaveNothing = true;
        return nullptr;
    }

    // Only a whole file: a partial decode would line up its start and
    // have nothing to say about its end.
    if (timedOut || unreadable || !done || decoder.error() != QAudioDecoder::NoError || !resampler) {
        return nullptr;
    }
    const std::vector<float> rest = resampler->finish();
    kept.append(rest);
    auto samples = std::make_shared<domain::alignment::KeptSamples>(kept.finish());
    if (samples->total == 0) {
        return nullptr;
    }
    return samples;
}

std::shared_ptr<const domain::alignment::KeptSamples> QtMultimediaAlignmentProbe::decode(
    const std::string &absoluteFilePath) const
{
    std::shared_ptr<const domain::alignment::KeptSamples> samples;
    decoder_start::run(absoluteFilePath, [&](const decoder_start::Attempt &attempt) {
        bool gaveNothing = false;
        samples = decodeOnce(absoluteFilePath, attempt, gaveNothing);
        return gaveNothing;
    });
    return samples;
}

}  // namespace seabass::infrastructure::audio
