// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Where an unmute lands in what the sound server actually plays (#92).
// Plays FILE through a QAudioOutput on the output device whose id is
// DEVICE, JUMPS times: each jump seeks and plays for 600 ms, then pauses
// for GAP_MS. MODE is
//
//   plain   seek to CUE_MS and play: what the player does today
//   tap     mute, seek 200 ms before the cue, unmute when the
//           QAudioBufferOutput delivers the buffer that holds the cue
//   clock   mute, seek 200 ms before the cue, unmute when the player's
//           clock reaches the cue minus $LEAD ms (polled every
//           millisecond; the clock is position() interpolated with the
//           steady time since its last report, see probe_common.hpp,
//           because position() itself steps in 50 ms)
//   warm    a decoder kept warm at the cue: mute, seek 200 ms before the
//           cue, play, pause when the clock reaches the cue minus $LEAD
//           ms, wait 300 ms, unmute, play on (no seek, so the bit
//           reservoir is intact)
//   warmtap the same, but paused when the QAudioBufferOutput delivers
//           the last buffer before the one that holds the cue
//   live    a jump while playing, as the app usually makes one: play
//           from $FROM ms (default 2700), and 300 ms later seek to the
//           cue without pausing. Meant for tonegap files, silent from
//           2.6 s on: what plays before the jump is silence, so any tone
//           before the click came from before the cue
//
// SOURCE=buffer in the environment reads FILE into memory and hands it to
// the player with setSourceDevice() (a QBuffer) instead of a URL.
//
// Prints one line per jump (when the unmute or the pause came, on the
// player's clock) to stdout. measure-unmute.sh records the device and
// analyse_unmute.py reads the recording. Built by hand, see README.md.
//
//   sink_probe FILE CUE_MS MODE JUMPS DEVICE [GAP_MS]

#include "probe_common.hpp"

#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QAudioOutput>
#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <cstdlib>
#include <functional>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 6) {
        std::fprintf(stderr, "usage: sink_probe FILE CUE_MS plain|tap|clock|warm|warmtap|live JUMPS DEVICE [GAP_MS]\n");
        return 2;
    }
    const QAudioDevice device = probe::outputDevice(argv[5]);
    if (device.isNull()) {
        return 1;
    }
    QMediaPlayer player;
    QAudioBufferOutput tap;
    QAudioOutput out;
    out.setDevice(device);
    player.setAudioOutput(&out);
    player.setAudioBufferOutput(&tap);
    probe::PlayerClock clock(player);
    const qint64 cue = std::atoll(argv[2]);
    const QString mode = QString::fromUtf8(argv[3]);
    const int jumps = std::atoi(argv[4]);
    const int gapMs = argc > 6 ? std::atoi(argv[6]) : 500;
    const qint64 lead = qEnvironmentVariableIntValue("LEAD");
    const bool known = mode == QLatin1String("plain") || mode == QLatin1String("tap") || mode == QLatin1String("clock")
        || mode == QLatin1String("warm") || mode == QLatin1String("warmtap") || mode == QLatin1String("live");
    if (!known) {
        std::fprintf(stderr, "unknown mode %s\n", argv[3]);
        return 2;
    }
    int jump = 0;
    bool armed = false;
    // Warm: held at the cue, muted, then let go as a hot cue would.
    const auto hold = [&] {
        std::printf("jump %d: paused at %.1f ms on the player's clock\n", jump, clock.positionMs());
        player.pause();
        QTimer::singleShot(300, &app, [&] {
            out.setMuted(false);
            player.play();
        });
    };
    QTimer poll;
    poll.setTimerType(Qt::PreciseTimer);
    poll.setInterval(1);
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        if (!armed || clock.positionMs() < cue - lead) {
            return;
        }
        armed = false;
        poll.stop();
        if (mode == QLatin1String("warm")) {
            hold();
            return;
        }
        std::printf("jump %d: unmuted at %.1f ms on the player's clock\n", jump, clock.positionMs());
        out.setMuted(false);
    });
    QObject::connect(&tap, &QAudioBufferOutput::audioBufferReceived, &app, [&](const QAudioBuffer &buffer) {
        if (!armed || !buffer.isValid()) {
            return;
        }
        const qint64 lengthUs = buffer.format().durationForFrames(buffer.frameCount());
        const qint64 endUs = buffer.startTime() + lengthUs;
        if (mode == QLatin1String("warmtap")) {
            if (endUs + lengthUs > cue * 1000) {
                armed = false;
                hold();
            }
            return;
        }
        if (mode == QLatin1String("tap") && endUs > cue * 1000) {
            std::printf("jump %d: unmuted on the buffer from %.1f ms\n", jump, buffer.startTime() / 1000.0);
            out.setMuted(false);
            armed = false;
        }
    });
    std::function<void()> doJump = [&] {
        if (jump++ >= jumps) {
            app.quit();
            return;
        }
        if (mode == QLatin1String("live")) {
            player.setPosition(qEnvironmentVariableIsSet("FROM") ? qEnvironmentVariableIntValue("FROM") : 2700);
            player.play();
            QTimer::singleShot(300, &app, [&] {
                player.setPosition(cue);
                QTimer::singleShot(600, &app, [&] {
                    player.pause();
                    QTimer::singleShot(gapMs, &app, doJump);
                });
            });
            return;
        }
        if (mode == QLatin1String("plain")) {
            player.setPosition(cue);
        } else {
            out.setMuted(true);
            armed = true;
            player.setPosition(qMax<qint64>(0, cue - 200));
        }
        clock.reset();
        if (mode == QLatin1String("clock") || mode == QLatin1String("warm")) {
            poll.start();
        }
        player.play();
        QTimer::singleShot(mode.startsWith(QLatin1String("warm")) ? 900 : 600, &app, [&] {
            player.pause();
            out.setMuted(false);
            QTimer::singleShot(gapMs, &app, doJump);
        });
    };
    QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &app, [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia && jump == 0) {
            std::printf("loaded: duration %lld ms\n", static_cast<long long>(player.duration()));
            QTimer::singleShot(300, &app, doJump);
        }
    });
    QObject::connect(&player, &QMediaPlayer::errorOccurred, &app, [&](QMediaPlayer::Error, const QString &message) {
        std::fprintf(stderr, "player error: %s\n", qPrintable(message));
        app.exit(1);
    });
    QBuffer memory;
    const QString file = QString::fromUtf8(argv[1]);
    if (qgetenv("SOURCE") == "buffer") {
        QFile in(file);
        if (!in.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "cannot read %s\n", argv[1]);
            return 1;
        }
        memory.setData(in.readAll());
        memory.open(QIODevice::ReadOnly);
        // The URL only names the data (FFmpeg probes the content itself).
        player.setSourceDevice(&memory, QUrl::fromLocalFile(file));
    } else {
        player.setSource(QUrl::fromLocalFile(file));
    }
    QTimer::singleShot(10000 + jumps * (1000 + gapMs), &app, [&] {
        std::fprintf(stderr, "timed out after jump %d\n", jump);
        app.exit(1);
    });
    return app.exec();
}
