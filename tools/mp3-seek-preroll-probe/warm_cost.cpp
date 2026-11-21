// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What a decoder kept warm at every hot cue costs (#92): COUNT extra
// QMediaPlayers on FILE, each with its own QAudioOutput on the device
// whose id is DEVICE, each played muted from 200 ms before its cue to the
// cue and paused there (cues 300 ms apart from 1500 ms on, so eight fit
// in the probe's four second files), the cue judged on the player's clock
// (probe_common.hpp: position() itself steps in 50 ms). Prints the
// process's resident memory and thread count before and after, and how
// long the preparation took. Linux only (reads /proc/self/status). Run it
// with measure-unmute.sh --run, which makes the null sink. Built by hand,
// see README.md.
//
//   warm_cost FILE COUNT DEVICE

#include "probe_common.hpp"

#include <QAudioOutput>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace
{

// std::ifstream, not QFile: a /proc file reports size 0, and QFile's
// atEnd() believes it.
QByteArray statusLine(const char *key)
{
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind(key, 0) == 0) {
            return QByteArray::fromStdString(line.substr(std::strlen(key))).trimmed();
        }
    }
    return "?";
}

void report(const char *when)
{
    std::printf("%s: VmRSS %s, Threads %s\n", when, statusLine("VmRSS:").constData(),
                statusLine("Threads:").constData());
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 4) {
        std::fprintf(stderr, "usage: warm_cost FILE COUNT DEVICE\n");
        return 2;
    }
    const QUrl file = QUrl::fromLocalFile(QString::fromUtf8(argv[1]));
    const int count = std::atoi(argv[2]);
    const QAudioDevice device = probe::outputDevice(argv[3]);
    if (device.isNull()) {
        return 1;
    }

    // The app's own player, loaded, as the baseline.
    QMediaPlayer main;
    QAudioOutput mainOutput;
    mainOutput.setDevice(device);
    mainOutput.setMuted(true);
    main.setAudioOutput(&mainOutput);
    main.setSource(file);

    struct Warm
    {
        std::unique_ptr<QMediaPlayer> player;
        std::unique_ptr<QAudioOutput> output;
        std::unique_ptr<probe::PlayerClock> clock;
        qint64 cue = 0;
        bool held = false;
    };
    std::vector<Warm> warm;
    QElapsedTimer clock;
    int held = 0;
    QTimer poll;
    poll.setTimerType(Qt::PreciseTimer);
    poll.setInterval(1);
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        for (Warm &w : warm) {
            if (!w.held && w.player->playbackState() == QMediaPlayer::PlayingState
                && w.clock->positionMs() >= w.cue) {
                w.player->pause();
                w.held = true;
                ++held;
            }
        }
        if (held == count) {
            poll.stop();
            std::printf("%d warm players held at their cues after %lld ms\n", count,
                        static_cast<long long>(clock.elapsed()));
            QTimer::singleShot(1000, &app, [&] {
                report("after");
                app.quit();
            });
        }
    });

    QObject::connect(&main, &QMediaPlayer::mediaStatusChanged, &app, [&](QMediaPlayer::MediaStatus status) {
        if (status != QMediaPlayer::LoadedMedia || !warm.empty()) {
            return;
        }
        QTimer::singleShot(500, &app, [&] {
            report("one player loaded");
            clock.start();
            for (int i = 0; i < count; ++i) {
                Warm w;
                w.player = std::make_unique<QMediaPlayer>();
                w.output = std::make_unique<QAudioOutput>();
                w.output->setDevice(device);
                w.output->setMuted(true);
                w.player->setAudioOutput(w.output.get());
                w.clock = std::make_unique<probe::PlayerClock>(*w.player);
                w.cue = 1500 + 300 * i;
                QMediaPlayer *player = w.player.get();
                probe::PlayerClock *playerClock = w.clock.get();
                const qint64 cue = w.cue;
                QObject::connect(player, &QMediaPlayer::mediaStatusChanged, &app,
                                 [player, playerClock, cue](QMediaPlayer::MediaStatus s) {
                                     if (s == QMediaPlayer::LoadedMedia
                                         && player->playbackState() == QMediaPlayer::StoppedState) {
                                         player->setPosition(cue - 200);
                                         playerClock->reset();
                                         player->play();
                                     }
                                 });
                w.player->setSource(file);
                warm.push_back(std::move(w));
            }
            if (count == 0) {
                QTimer::singleShot(1000, &app, [&] {
                    report("after");
                    app.quit();
                });
            } else {
                poll.start();
            }
        });
    });
    QTimer::singleShot(30000, &app, [&] {
        std::fprintf(stderr, "timed out with %d of %d held\n", held, count);
        app.exit(1);
    });
    return app.exec();
}
