// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Where the first sound after a seek comes out of the player (#92):
// QMediaPlayer with a QAudioBufferOutput and no audio output (so no
// sound card is needed), loads FILE, seeks to SEEK_MS, plays, and prints
// the start of the first buffer delivered and the time of the first
// sample above 0.05. Built by hand, see README.md.
//
//   seek_probe FILE SEEK_MS

#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QCoreApplication>
#include <QMediaPlayer>
#include <QTimer>
#include <QUrl>

#include <cmath>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "usage: seek_probe FILE SEEK_MS\n");
        return 2;
    }
    QMediaPlayer player;
    QAudioBufferOutput tap;
    player.setAudioBufferOutput(&tap);
    const qint64 seekMs = std::atoll(argv[2]);
    bool started = false;
    double streamFrom = -1;
    double firstSound = -1;
    QObject::connect(&tap, &QAudioBufferOutput::audioBufferReceived, &app, [&](const QAudioBuffer &buffer) {
        if (!buffer.isValid() || !started) {
            return;
        }
        if (streamFrom < 0) {
            streamFrom = buffer.startTime() / 1000.0;
        }
        if (firstSound >= 0) {
            return;
        }
        const QAudioFormat format = buffer.format();
        for (qsizetype i = 0; i < buffer.frameCount(); ++i) {
            float v = 0;
            const qsizetype at = i * format.channelCount();
            switch (format.sampleFormat()) {
            case QAudioFormat::Float: v = buffer.constData<float>()[at]; break;
            case QAudioFormat::Int16: v = buffer.constData<qint16>()[at] / 32768.0F; break;
            case QAudioFormat::Int32: v = static_cast<float>(buffer.constData<qint32>()[at] / 2147483648.0); break;
            default: break;
            }
            if (std::fabs(v) > 0.05F) {
                firstSound = buffer.startTime() / 1000.0 + i * 1000.0 / format.sampleRate();
                break;
            }
        }
    });
    QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &app, [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia && !started) {
            started = true;
            player.setPosition(seekMs);
            player.play();
            QTimer::singleShot(1500, &app, [&] {
                std::printf("seek %lld: stream from %.3f ms, first sound %.3f ms\n",
                            static_cast<long long>(seekMs), streamFrom, firstSound);
                app.quit();
            });
        }
    });
    QObject::connect(&player, &QMediaPlayer::errorOccurred, &app, [&](QMediaPlayer::Error, const QString &message) {
        std::printf("error: %s\n", qPrintable(message));
        app.exit(1);
    });
    player.setSource(QUrl::fromLocalFile(QString::fromUtf8(argv[1])));
    QTimer::singleShot(10000, &app, [&] { app.exit(1); });
    return app.exec();
}
