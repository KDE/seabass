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
        std::fprintf(stderr, "usage: sink_probe FILE CUE_MS plain|tap|clock JUMPS DEVICE [GAP_MS]\n");
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
    QTimer poll;
    poll.setTimerType(Qt::PreciseTimer);
    poll.setInterval(1);
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        if (armed && player.position() >= cue - lead) {
            out.setMuted(false);
            armed = false;
            poll.stop();
        }
    });
    QObject::connect(&tap, &QAudioBufferOutput::audioBufferReceived, &app, [&](const QAudioBuffer &buffer) {
        if (!armed || mode != QLatin1String("tap") || !buffer.isValid()) {
            return;
        }
        const qint64 endUs = buffer.startTime() + buffer.format().durationForFrames(buffer.frameCount());
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
            if (mode == QLatin1String("clock")) {
                poll.start();
            }
        }
        player.play();
        QTimer::singleShot(600, &app, [&] {
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
