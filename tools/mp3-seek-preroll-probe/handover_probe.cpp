// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Can a hot cue's first milliseconds come from a buffer decoded ahead of
// time, and the player take over after them (#92)? FILE is decoded whole
// with QAudioDecoder (no seek, so the bit reservoir is intact at the cue)
// into memory at 48 kHz. Each jump then does two things at once:
//
//   - the QMediaPlayer, unmuted, seeks to CUE_MS and plays, as the player
//     does today: after the seek its MP3 decoder delivers silence for a
//     while, then the real audio;
//   - a QAudioSink of the probe's own (callback API, same device) plays
//     the decoded audio from CUE_MS on, and stops, sample-exact with a
//     2 ms fade, where the player's own audio begins: the first sample
//     above 0.075 (half the probe file's tone, the threshold
//     analyse_handover.py counts a gap by) its QAudioBufferOutput
//     delivers after the seek.
//
// If the two streams were aligned the recording would show the click on
// the cue and the tone running on without a seam. A gap or a doubled
// stretch at the splice is the misalignment. Every jump runs 1200 ms so
// the next click (CUE_MS + 1000 in the probe file) is the player's: its
// distance from the first click, minus 1000 ms, is how much later the
// player's stream sounds than the probe's for the same track time.
// analyse_handover.py reads the recording.
//
// DELAY=<ms> (fractions allowed) starts the own audio that much later
// than the player's seek, on the steady clock and to the sample: the
// audio callback works out where in the buffer it is filling that moment
// falls and starts there, so the start is not rounded to the sound
// server's period.
//
// Prints per jump where the splice is, when the tap told, how far the own
// stream had played by then (a splice it had already passed is counted
// as late: the own stream then stops at once, with the fade), and how
// late the own start was. Plays only on the output device whose id is
// DEVICE (measure-unmute.sh's null sink). Built by hand, see README.md.
//
//   handover_probe FILE CUE_MS JUMPS DEVICE [GAP_MS]

#include "probe_common.hpp"

#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QAudioDecoder>
#include <QAudioOutput>
#include <QAudioSink>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSpan>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <vector>

namespace
{

constexpr int Rate = 48000;
constexpr qint64 FadeFrames = Rate / 500;  // 2 ms
constexpr qint64 NoSplice = std::numeric_limits<qint64>::max();
constexpr float Threshold = 0.075F;

qint64 steadyNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

float sampleAt(const QAudioBuffer &buffer, qsizetype frame)
{
    const QAudioFormat format = buffer.format();
    const qsizetype at = frame * format.channelCount();
    switch (format.sampleFormat()) {
    case QAudioFormat::Float: return buffer.constData<float>()[at];
    case QAudioFormat::Int16: return buffer.constData<qint16>()[at] / 32768.0F;
    case QAudioFormat::Int32: return static_cast<float>(buffer.constData<qint32>()[at] / 2147483648.0);
    default: return 0;
    }
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 5) {
        std::fprintf(stderr, "usage: handover_probe FILE CUE_MS JUMPS DEVICE [GAP_MS]\n");
        return 2;
    }
    const QString file = QString::fromUtf8(argv[1]);
    const qint64 cue = std::atoll(argv[2]);
    const int jumps = std::atoi(argv[3]);
    const int gapMs = argc > 5 ? std::atoi(argv[5]) : 400;
    const double delayMs = qEnvironmentVariable("DELAY", QStringLiteral("0")).toDouble();

    const QAudioDevice device = probe::outputDevice(argv[4]);
    if (device.isNull()) {
        return 1;
    }

    QAudioFormat format;
    format.setSampleRate(Rate);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Float);

    // 1. The whole file, decoded from its start.
    std::vector<float> decoded;
    {
        QAudioDecoder decoder;
        decoder.setAudioFormat(format);
        decoder.setSource(QUrl::fromLocalFile(file));
        QElapsedTimer clock;
        bool failed = false;
        QObject::connect(&decoder, &QAudioDecoder::bufferReady, &app, [&] {
            const QAudioBuffer buffer = decoder.read();
            if (buffer.format() != format) {
                std::fprintf(stderr, "the decoder delivered another format than the one asked for\n");
                failed = true;
                app.quit();
                return;
            }
            const qint64 first = buffer.startTime() * Rate / 1000000;
            const qint64 end = first + buffer.frameCount();
            if (first < 0) {
                return;
            }
            if (static_cast<qint64>(decoded.size()) < end * 2) {
                decoded.resize(static_cast<std::size_t>(end * 2), 0.0F);
            }
            const float *data = buffer.constData<float>();
            std::copy(data, data + buffer.frameCount() * 2, decoded.begin() + first * 2);
        });
        QObject::connect(&decoder, &QAudioDecoder::finished, &app, [&] { app.quit(); });
        QObject::connect(&decoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), &app, [&] {
            std::fprintf(stderr, "decode failed: %s\n", qPrintable(decoder.errorString()));
            failed = true;
            app.quit();
        });
        clock.start();
        decoder.start();
        app.exec();
        if (failed) {
            return 1;
        }
        const qint64 frames = static_cast<qint64>(decoded.size() / 2);
        std::printf("decoded %.1f s of audio in %lld ms\n", frames / double(Rate),
                    static_cast<long long>(clock.elapsed()));
        for (qint64 i = cue * Rate / 1000 - Rate / 10; i < frames; ++i) {
            if (i >= 0 && std::fabs(decoded[static_cast<std::size_t>(i * 2)]) > 0.6F) {
                std::printf("decoded click at %.3f ms (the player's timeline should agree)\n", i * 1000.0 / Rate);
                break;
            }
        }
    }
    const qint64 cueFrame = cue * Rate / 1000;
    const qint64 decodedFrames = static_cast<qint64>(decoded.size() / 2);
    if (cueFrame >= decodedFrames) {
        std::fprintf(stderr, "cue past the end of the file\n");
        return 1;
    }

    // 2. The own sink. Everything it reads is set before a jump starts it
    // or is atomic: the callback does not lock and does not allocate.
    std::atomic<qint64> startAtNs{0};         // 0: silent
    std::atomic<qint64> spliceAt{NoSplice};   // frames after the cue
    std::atomic<qint64> playedFrames{0};      // for the log
    std::atomic<qint64> startLateNs{0};       // for the log
    qint64 played = 0;                        // audio thread only
    bool started = false;                     // audio thread only
    QAudioSink sink(device, format);
    sink.start([&](QSpan<float> out) {
        const qsizetype frames = out.size() / 2;
        std::fill(out.begin(), out.end(), 0.0F);
        const qint64 startAt = startAtNs.load(std::memory_order_acquire);
        if (startAt == 0) {
            played = 0;
            started = false;
            playedFrames.store(0, std::memory_order_relaxed);
            return;
        }
        qsizetype from = 0;
        if (!started) {
            const qint64 now = steadyNs();
            const qint64 offset = (startAt - now) * Rate / 1000000000;
            if (offset >= frames) {
                return;  // not in this buffer yet
            }
            from = offset > 0 ? static_cast<qsizetype>(offset) : 0;
            startLateNs.store(offset < 0 ? now - startAt : 0, std::memory_order_relaxed);
            started = true;
        }
        const qint64 splice = spliceAt.load(std::memory_order_acquire);
        for (qsizetype i = from; i < frames; ++i) {
            const qint64 at = cueFrame + played;
            float gain = 1.0F;
            if (played >= splice) {
                gain = played - splice >= FadeFrames ? 0.0F : 1.0F - float(played - splice) / FadeFrames;
            }
            if (at < decodedFrames) {
                out[i * 2] = decoded[static_cast<std::size_t>(at * 2)] * gain;
                out[i * 2 + 1] = decoded[static_cast<std::size_t>(at * 2 + 1)] * gain;
            }
            ++played;
        }
        playedFrames.store(played, std::memory_order_relaxed);
    });
    if (sink.error() != QtAudio::NoError) {
        std::fprintf(stderr, "own sink did not start (error %d)\n", int(sink.error()));
        return 1;
    }

    // 3. The player, as the app has it, with the tap that finds where its
    // own audio begins after the seek.
    QMediaPlayer player;
    QAudioOutput output;
    output.setDevice(device);
    QAudioBufferOutput tap;
    player.setAudioOutput(&output);
    player.setAudioBufferOutput(&tap);
    bool looking = false;
    int late = 0;
    QElapsedTimer sinceJump;
    QObject::connect(&tap, &QAudioBufferOutput::audioBufferReceived, &app, [&](const QAudioBuffer &buffer) {
        if (!looking || !buffer.isValid()) {
            return;
        }
        const double startMs = buffer.startTime() / 1000.0;
        const int rate = buffer.format().sampleRate();
        for (qsizetype i = 0; i < buffer.frameCount(); ++i) {
            if (std::fabs(sampleAt(buffer, i)) > Threshold) {
                const double atMs = startMs + i * 1000.0 / rate;
                if (atMs < cue) {
                    continue;  // a frame from before the cue, if one comes
                }
                const qint64 splice = std::llround((atMs - cue) * Rate / 1000.0);
                spliceAt.store(splice, std::memory_order_release);
                const qint64 playedNow = playedFrames.load(std::memory_order_relaxed);
                looking = false;
                if (playedNow > splice) {
                    ++late;
                }
                std::printf("jump: player's audio from %.2f ms after the cue, tap told %lld ms after the jump, "
                            "own stream had played %.2f ms%s\n", atMs - cue,
                            static_cast<long long>(sinceJump.elapsed()), playedNow * 1000.0 / Rate,
                            playedNow > splice ? " (LATE: past the splice)" : "");
                return;
            }
        }
    });

    int jump = 0;
    std::function<void()> doJump = [&] {
        if (jump > 0) {
            std::printf("jump: own start %.3f ms late\n",
                        startLateNs.load(std::memory_order_relaxed) / 1e6);
        }
        if (jump++ >= jumps) {
            std::printf("late splices: %d of %d\n", late, jumps);
            app.quit();
            return;
        }
        spliceAt.store(NoSplice, std::memory_order_release);
        playedFrames.store(0, std::memory_order_relaxed);
        looking = true;
        sinceJump.start();
        player.setPosition(cue);
        player.play();
        startAtNs.store(steadyNs() + static_cast<qint64>(delayMs * 1e6), std::memory_order_release);
        QTimer::singleShot(1200, &app, [&] {
            player.pause();
            startAtNs.store(0, std::memory_order_release);
            if (looking) {
                std::printf("jump: the player's audio never came\n");
            }
            looking = false;
            QTimer::singleShot(gapMs, &app, doJump);
        });
    };
    QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &app, [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia && jump == 0) {
            QTimer::singleShot(300, &app, doJump);
        }
    });
    QObject::connect(&player, &QMediaPlayer::errorOccurred, &app, [&](QMediaPlayer::Error, const QString &message) {
        std::fprintf(stderr, "player error: %s\n", qPrintable(message));
        app.exit(1);
    });
    player.setSource(QUrl::fromLocalFile(file));
    const int result = app.exec();
    sink.stop();
    return result;
}
