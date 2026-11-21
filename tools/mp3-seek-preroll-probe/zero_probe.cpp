// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Does the player's position() mean the same sound for an MP3 and for the
// WAV decoded from it, when either is played from 0 (#92)? align_probe
// found the QAudioBufferOutput labels of a LAME MP3 played from the start
// one start offset (25 ms) early; labels are not what is heard, and
// position() is the clock the cues, the ring and the popup run on.
//
// Each run seeks FILE to 0 and plays it. When the player's clock
// (probe_common.hpp) reads MARK_MS, a QAudioSink of the probe's own on
// the same device plays a marker at once (2 ms at -0.9, placed to the
// sample in its callback, so its own latency is the same every run). The
// recording then holds the marker and, later, the file's click: the
// distance from marker to click is MARK_MS's distance to the click on
// the player's clock, plus constants that are the same for both sources.
// Run it on the MP3 and on the WAV and compare: a different position()
// shows as a different distance. analyse_zero.py reads the recording.
// Plays only on the output device whose id is DEVICE (measure-unmute.sh's
// null sink). Built by hand, see README.md.
//
//   zero_probe FILE MARK_MS RUNS DEVICE [GAP_MS]

#include "probe_common.hpp"

#include <QAudioOutput>
#include <QAudioSink>
#include <QCoreApplication>
#include <QSpan>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace
{

constexpr int Rate = 48000;
constexpr qint64 MarkerFrames = Rate / 500;  // 2 ms

qint64 steadyNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 5) {
        std::fprintf(stderr, "usage: zero_probe FILE MARK_MS RUNS DEVICE [GAP_MS]\n");
        return 2;
    }
    const qint64 mark = std::atoll(argv[2]);
    const int runs = std::atoi(argv[3]);
    const int gapMs = argc > 5 ? std::atoi(argv[5]) : 400;
    const QAudioDevice device = probe::outputDevice(argv[4]);
    if (device.isNull()) {
        return 1;
    }

    QAudioFormat format;
    format.setSampleRate(Rate);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Float);
    std::atomic<qint64> markAtNs{0};  // 0: silent
    qint64 left = 0;                  // audio thread only
    QAudioSink sink(device, format);
    sink.start([&](QSpan<float> out) {
        std::fill(out.begin(), out.end(), 0.0F);
        const qsizetype frames = out.size() / 2;
        qsizetype from = 0;
        if (left == 0) {
            const qint64 at = markAtNs.load(std::memory_order_acquire);
            if (at == 0) {
                return;
            }
            const qint64 offset = (at - steadyNs()) * Rate / 1000000000;
            if (offset >= frames) {
                return;
            }
            from = offset > 0 ? static_cast<qsizetype>(offset) : 0;
            left = MarkerFrames;
            markAtNs.store(0, std::memory_order_release);
        }
        for (qsizetype i = from; i < frames && left > 0; ++i, --left) {
            out[i * 2] = -0.9F;
            out[i * 2 + 1] = -0.9F;
        }
    });
    if (sink.error() != QtAudio::NoError) {
        std::fprintf(stderr, "own sink did not start (error %d)\n", int(sink.error()));
        return 1;
    }

    QMediaPlayer player;
    QAudioOutput output;
    output.setDevice(device);
    player.setAudioOutput(&output);
    probe::PlayerClock clock(player);
    int run = 0;
    bool armed = false;
    QTimer poll;
    poll.setTimerType(Qt::PreciseTimer);
    poll.setInterval(1);
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        if (armed && clock.positionMs() >= mark) {
            armed = false;
            poll.stop();
            markAtNs.store(steadyNs() + 30000000, std::memory_order_release);  // 30 ms on: in a later buffer
            std::printf("run %d: marker at %.2f ms on the player's clock (position() %lld)\n", run,
                        clock.positionMs() + 30.0, static_cast<long long>(player.position()));
        }
    });
    std::function<void()> next = [&] {
        if (run++ >= runs) {
            app.quit();
            return;
        }
        player.setPosition(0);
        clock.reset();
        armed = true;
        poll.start();
        player.play();
        QTimer::singleShot(static_cast<int>(mark) + 600, &app, [&] {
            player.pause();
            QTimer::singleShot(gapMs, &app, next);
        });
    };
    QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &app, [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia && run == 0) {
            QTimer::singleShot(300, &app, next);
        }
    });
    QObject::connect(&player, &QMediaPlayer::errorOccurred, &app, [&](QMediaPlayer::Error, const QString &message) {
        std::fprintf(stderr, "player error: %s\n", qPrintable(message));
        app.exit(1);
    });
    player.setSource(QUrl::fromLocalFile(QString::fromUtf8(argv[1])));
    const int result = app.exec();
    sink.stop();
    return result;
}
