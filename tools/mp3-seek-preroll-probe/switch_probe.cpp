// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What switching a playing track from its MP3 to the WAV decoded from it
// sounds like (#92: playback could start on the MP3 at once and move to
// the PCM when the decode is done). Each jump plays MP3 from 1300 ms (so
// its click at 1500 is heard), and 500 ms after play() switches to WAV
// at the position the player's clock (probe_common.hpp) says is being
// heard then, plus $AHEAD ms. The WAV's click at 2500 is then heard from
// the second source: analyse_handover.py reads the offset between the
// two (how much later the WAV sounds than the MP3 would have) and the gap.
//
// SWITCH=two (the default) has a second QMediaPlayer with the WAV loaded
// and paused, started and the first one stopped at the switch;
// SWITCH=same gives the one player the WAV with setSource() and seeks it
// once it has loaded. Each jump plays 1500 ms, then GAP_MS of silence.
// Plays only on the output device whose id is DEVICE (measure-unmute.sh's
// null sink). Built by hand, see README.md.
//
//   switch_probe MP3 WAV JUMPS DEVICE [GAP_MS]

#include "probe_common.hpp"

#include <QAudioOutput>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QUrl>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 5) {
        std::fprintf(stderr, "usage: switch_probe MP3 WAV JUMPS DEVICE [GAP_MS]\n");
        return 2;
    }
    const QUrl mp3 = QUrl::fromLocalFile(QString::fromUtf8(argv[1]));
    const QUrl wav = QUrl::fromLocalFile(QString::fromUtf8(argv[2]));
    const int jumps = std::atoi(argv[3]);
    const int gapMs = argc > 5 ? std::atoi(argv[5]) : 400;
    const double ahead = qEnvironmentVariable("AHEAD", QStringLiteral("0")).toDouble();
    const bool same = qgetenv("SWITCH") == "same";
    const QAudioDevice device = probe::outputDevice(argv[4]);
    if (device.isNull()) {
        return 1;
    }

    QMediaPlayer first;
    QAudioOutput firstOut;
    firstOut.setDevice(device);
    first.setAudioOutput(&firstOut);
    probe::PlayerClock clock(first);
    QMediaPlayer second;
    QAudioOutput secondOut;
    secondOut.setDevice(device);
    second.setAudioOutput(&secondOut);

    int jump = 0;
    int loaded = 0;
    qint64 switchTo = 0;
    bool awaitingWav = false;
    bool wavLoading = false;
    QElapsedTimer sinceSwitch;
    // SWITCH=same: play the MP3, and give the player the WAV 500 ms in.
    const auto sameJump = [&] {
        first.setPosition(1300);
        clock.reset();
        first.play();
        QTimer::singleShot(500, Qt::PreciseTimer, &app, [&] {
            switchTo = std::llround(clock.positionMs() + ahead);
            awaitingWav = true;
            sinceSwitch.start();
            first.setSource(wav);
        });
    };
    std::function<void()> doJump = [&] {
        if (jump++ >= jumps) {
            app.quit();
            return;
        }
        if (same) {
            if (first.source() == mp3) {
                sameJump();
            } else {
                first.setSource(mp3);  // the jump goes on once it has loaded (below)
            }
            return;
        }
        first.setPosition(1300);
        clock.reset();
        first.play();
        QTimer::singleShot(500, Qt::PreciseTimer, &app, [&] {
            switchTo = std::llround(clock.positionMs() + ahead);
            second.setPosition(switchTo);
            second.play();
            first.stop();
            std::printf("jump %d: switched at %lld ms\n", jump, static_cast<long long>(switchTo));
            QTimer::singleShot(1000, &app, [&] {
                second.pause();
                QTimer::singleShot(gapMs, &app, doJump);
            });
        });
    };
    QObject::connect(&first, &QMediaPlayer::mediaStatusChanged, &app, [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadingMedia && awaitingWav) {
            wavLoading = true;
        }
        if (status != QMediaPlayer::LoadedMedia) {
            return;
        }
        if (!same) {
            if (++loaded == 2) {
                QTimer::singleShot(300, &app, doJump);
            }
            return;
        }
        if (awaitingWav) {
            // The same player, now on the WAV (not the status the old
            // source reports as setSource() stops it).
            if (!wavLoading) {
                return;
            }
            awaitingWav = false;
            wavLoading = false;
            first.setPosition(switchTo);
            first.play();
            std::printf("jump %d: switched at %lld ms, WAV playing %lld ms after the switch\n", jump,
                        static_cast<long long>(switchTo), static_cast<long long>(sinceSwitch.elapsed()));
            QTimer::singleShot(1000, &app, [&] {
                first.pause();
                QTimer::singleShot(gapMs, &app, doJump);
            });
            return;
        }
        if (jump == 0) {
            QTimer::singleShot(300, &app, doJump);
            return;
        }
        if (first.source() == mp3 && first.playbackState() == QMediaPlayer::StoppedState) {
            sameJump();
        }
    });
    QObject::connect(&second, &QMediaPlayer::mediaStatusChanged, &app, [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia && !same && ++loaded == 2) {
            QTimer::singleShot(300, &app, doJump);
        }
    });
    for (QMediaPlayer *player : {&first, &second}) {
        QObject::connect(player, &QMediaPlayer::errorOccurred, &app, [&](QMediaPlayer::Error, const QString &message) {
            std::fprintf(stderr, "player error: %s\n", qPrintable(message));
            app.exit(1);
        });
    }
    first.setSource(mp3);
    if (!same) {
        second.setSource(wav);
    }
    QTimer::singleShot(10000 + jumps * (2500 + gapMs), &app, [&] {
        std::fprintf(stderr, "timed out after jump %d\n", jump);
        app.exit(1);
    });
    return app.exec();
}
