// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Where an unmute lands in what the sound server actually plays (#92).
// Plays FILE through a QAudioOutput on the output device whose id or
// description contains DEVICE, JUMPS times: each jump seeks and plays for
// 600 ms, then pauses for GAP_MS. MODE is
//
//   plain  seek to CUE_MS and play: what the player does today
//   tap    mute, seek 200 ms before the cue, unmute when the
//          QAudioBufferOutput delivers the buffer that holds the cue
//   clock  mute, seek 200 ms before the cue, unmute when
//          QMediaPlayer::position() reaches the cue minus $LEAD ms
//          (polled every millisecond on a precise timer)
//   warm   a decoder kept warm at the cue: mute, seek 200 ms before the
//          cue, play, pause when position() reaches the cue minus $LEAD
//          ms, wait 300 ms, unmute, play on (no seek, so the bit
//          reservoir is intact)
//   warmtap the same, but paused when the QAudioBufferOutput delivers
//          the last buffer before the one that holds the cue: the
//          player then stops feeding its sink one MP3 frame before the
//          cue's
//
// measure-unmute.sh records the device and analyse_unmute.py reads the
// recording. Built by hand, see README.md.
//
//   sink_probe FILE CUE_MS MODE JUMPS DEVICE [GAP_MS]

#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QAudioDevice>
#include <QAudioOutput>
#include <QCoreApplication>
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <cstdlib>
#include <functional>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 6) {
        std::fprintf(stderr, "usage: sink_probe FILE CUE_MS plain|tap|clock|warm|warmtap JUMPS DEVICE [GAP_MS]\n");
        return 2;
    }
    QMediaPlayer player;
    QAudioBufferOutput tap;
    QAudioOutput out;
    bool found = false;
    for (const QAudioDevice &device : QMediaDevices::audioOutputs()) {
        if (QString::fromUtf8(device.id()).contains(QString::fromUtf8(argv[5]))
            || device.description().contains(QString::fromUtf8(argv[5]))) {
            out.setDevice(device);
            found = true;
        }
    }
    if (!found) {
        std::fprintf(stderr, "no output device matches %s\n", argv[5]);
        return 1;
    }
    player.setAudioOutput(&out);
    player.setAudioBufferOutput(&tap);
    const qint64 cue = std::atoll(argv[2]);
    const QString mode = QString::fromUtf8(argv[3]);
    const int jumps = std::atoi(argv[4]);
    const int gapMs = argc > 6 ? std::atoi(argv[6]) : 500;
    const qint64 lead = qEnvironmentVariableIntValue("LEAD");
    int jump = 0;
    bool armed = false;
    // Warm: held at the cue, muted, then let go as a hot cue would.
    const auto hold = [&] {
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
        if (armed && player.position() >= cue - lead) {
            armed = false;
            poll.stop();
            if (mode != QLatin1String("warm")) {
                out.setMuted(false);
                return;
            }
            hold();
        }
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
        if (mode != QLatin1String("tap")) {
            return;
        }
        if (endUs > cue * 1000) {
            out.setMuted(false);
            armed = false;
        }
    });
    std::function<void()> doJump = [&] {
        if (jump++ >= jumps) {
            app.quit();
            return;
        }
        if (mode == QLatin1String("plain")) {
            player.setPosition(cue);
        } else {
            out.setMuted(true);
            armed = true;
            player.setPosition(qMax<qint64>(0, cue - 200));
            if (mode == QLatin1String("clock") || mode == QLatin1String("warm")) {
                poll.start();
            }
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
            QTimer::singleShot(300, &app, doJump);
        }
    });
    player.setSource(QUrl::fromLocalFile(QString::fromUtf8(argv[1])));
    return app.exec();
}
