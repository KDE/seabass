// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The decoder Seabass plays with, against the click files in
// tests/fixtures/mp3-click: where QAudioDecoder (Qt Multimedia's FFmpeg
// backend, the same demuxer and codec the player uses) puts a click that
// is at 1.500 s in the encoded PCM.
//
// playback_timeline.hpp rests on this: the player's timeline is Engine's,
// which domain::Mp3LeadIn models as Mp3Decoder::EngineFfmpegGapless. So
// for every file the click must land at
//
//     1.500 s + 576 + 529 + samplesBeforeAudio(leadIn, EngineFfmpegGapless)
//
// 576 being the delay libmp3lame put in front of the audio in every
// fixture (what a LAME or Lavc tag declares, and what a tagless file
// carries just the same), 529 the MP3 decoder's own output delay. A LAME
// or Lavc tag makes the decoder trim both (the sum is then 0 and the
// click lands on 1.500 s exactly); without one nothing is trimmed and
// the click comes 1105 samples late. mp3_decoder_offset_check measures
// libavformat directly and finds the same; this measures it through Qt.
//
//   qt_decoder_timeline_test FIXTURE_DIR
//
// No decoding backend is a FAIL, not a skip (docs/testing.md).

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "domain/mp3_lead_in.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"

using seabass::domain::Mp3Decoder;
using seabass::domain::samplesBeforeAudio;
using seabass::infrastructure::audio::readMp3LeadIn;

namespace
{

constexpr int LibMp3LameDelay = 576;
constexpr int DecoderDelay = 529;
constexpr double ClickAtSeconds = 1.5;
constexpr long ToleranceSamples = 2;

struct Decoded
{
    std::vector<float> mono;
    int sampleRate = 0;
    std::string error;
};

Decoded decode(const std::string &path)
{
    Decoded out;
    QAudioDecoder decoder;
    if (!decoder.isSupported()) {
        out.error = "no audio decoding backend (QAudioDecoder::isSupported() is false)";
        return out;
    }
    QAudioFormat wanted;
    wanted.setSampleFormat(QAudioFormat::Float);
    wanted.setChannelCount(1);
    decoder.setAudioFormat(wanted);
    decoder.setSource(QUrl::fromLocalFile(QString::fromStdString(path)));

    QEventLoop loop;
    bool done = false;
    QObject::connect(&decoder, &QAudioDecoder::bufferReady, &loop, [&]() {
        while (decoder.bufferAvailable()) {
            const QAudioBuffer buffer = decoder.read();
            if (!buffer.isValid()) {
                continue;
            }
            const QAudioFormat format = buffer.format();
            if (format.sampleFormat() != QAudioFormat::Float) {
                out.error = "the backend did not deliver Float samples";
                done = true;
                loop.quit();
                return;
            }
            out.sampleRate = format.sampleRate();
            const int channels = format.channelCount();
            const float *data = buffer.constData<float>();
            const qsizetype frames = buffer.frameCount();
            for (qsizetype frame = 0; frame < frames; ++frame) {
                double sum = 0.0;
                for (int channel = 0; channel < channels; ++channel) {
                    sum += data[frame * channels + channel];
                }
                out.mono.push_back(static_cast<float>(sum / channels));
            }
        }
    });
    QObject::connect(&decoder, &QAudioDecoder::finished, &loop, [&]() {
        done = true;
        loop.quit();
    });
    QObject::connect(&decoder, QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error), &loop,
                     [&](QAudioDecoder::Error) {
                         out.error = decoder.errorString().toStdString();
                         done = true;
                         loop.quit();
                     });
    QTimer::singleShot(30000, &loop, [&]() {
        out.error = "timed out";
        done = true;
        loop.quit();
    });
    decoder.start();
    if (!done) {
        loop.exec();
    }
    return out;
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::cerr << "usage: qt_decoder_timeline_test FIXTURE_DIR\n";
        return 2;
    }
    const std::string dir = std::string(argv[1]) + "/";
    const char *const files[] = {
        "click-A-notag-44100.mp3",      "click-B-noencoder-44100.mp3",  "click-B-lavc-44100.mp3",
        "click-C-badcrc-44100.mp3",     "click-D-lame-44100.mp3",       "click-D-lame-48000.mp3",
        "click-D-lame-mono-44100.mp3",  "click-D-lame-mpeg2-22050.mp3", "click-D-lame-crc-44100.mp3",
        "click-VBRI-44100.mp3",         "click-VBRI-modediffers-44100.mp3",
    };

    int failures = 0;
    for (const char *file : files) {
        const std::string path = dir + file;
        const Decoded decoded = decode(path);
        if (!decoded.error.empty()) {
            std::cout << "FAIL " << file << ": " << decoded.error << "\n";
            ++failures;
            continue;
        }
        const auto leadIn = readMp3LeadIn(path);
        if (!leadIn || leadIn->sampleRate != decoded.sampleRate) {
            std::cout << "FAIL " << file << ": lead-in unreadable or its rate is not the decoder's\n";
            ++failures;
            continue;
        }
        // The click: the largest magnitude in the first two seconds.
        const std::size_t window = std::min(decoded.mono.size(), static_cast<std::size_t>(2 * decoded.sampleRate));
        long clickIndex = -1;
        float peak = 0.0F;
        for (std::size_t i = 0; i < window; ++i) {
            const float magnitude = std::fabs(decoded.mono[i]);
            if (magnitude > peak) {
                peak = magnitude;
                clickIndex = static_cast<long>(i);
            }
        }
        const long clickAt = std::lround(ClickAtSeconds * decoded.sampleRate);
        const long expected = clickAt + LibMp3LameDelay + DecoderDelay
            + samplesBeforeAudio(*leadIn, Mp3Decoder::EngineFfmpegGapless);
        const bool pass = peak > 0.25F && std::labs(clickIndex - expected) <= ToleranceSamples;
        std::cout << (pass ? "PASS " : "FAIL ") << file << ": click at " << clickIndex << " (lead "
                  << (clickIndex - clickAt) << "), expected " << expected << ", peak " << peak << "\n";
        if (!pass) {
            ++failures;
        }
    }
    if (failures > 0) {
        std::cout << failures << " of " << std::size(files) << " files FAILED\n";
        return 1;
    }
    std::cout << "qt_decoder_timeline_test: all " << std::size(files) << " files passed\n";
    return 0;
}
