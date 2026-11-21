// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Does the WAV pcm_probe wrote play the same audio at the same position
// as the MP3 it came from (#92, and the start offset of issue #50)? For
// each time T: a QMediaPlayer with only a QAudioBufferOutput (no sound
// device) plays SOURCE from 400 ms before T at four times the speed
// (the buffers are unchanged, see README section 8), and the 100 ms of
// its output from T on, placed by the buffers' own timestamps, are
// compared with the WAV's samples from T * rate on, at every lag within
// +-60 ms. Prints the best lag in samples (0: the timelines agree to the
// sample), the mean difference there, and the duration() the player
// reports for SOURCE. Run it with the MP3 as SOURCE, and with the WAV
// itself as a control. T_MS@SEEK_MS seeks to SEEK_MS instead of 400 ms
// before T (0: play from the start, as when a track is simply played). A
// window with no sound in it says so and gives no lag. Built by hand, see
// README.md.
//
//   align_probe SOURCE WAV T_MS[@SEEK_MS]...

#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QCoreApplication>
#include <QFile>
#include <QMediaPlayer>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

namespace
{

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
    if (argc < 4) {
        std::fprintf(stderr, "usage: align_probe SOURCE WAV T_MS...\n");
        return 2;
    }
    // The WAV, left channel. pcm_probe writes a plain 44-byte header.
    QFile wavFile(QString::fromUtf8(argv[2]));
    if (!wavFile.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot read %s\n", argv[2]);
        return 1;
    }
    const QByteArray wav = wavFile.readAll();
    if (wav.size() < 44 || std::memcmp(wav.constData() + 36, "data", 4) != 0) {
        std::fprintf(stderr, "%s is not a WAV from pcm_probe\n", argv[2]);
        return 1;
    }
    int rate = 0;
    std::memcpy(&rate, wav.constData() + 24, 4);
    const auto *pcm = reinterpret_cast<const qint16 *>(wav.constData() + 44);
    const qint64 wavFrames = (wav.size() - 44) / 4;
    const auto wavAt = [&](qint64 frame) { return frame >= 0 && frame < wavFrames ? pcm[frame * 2] / 32768.0F : 0.0F; };

    QMediaPlayer player;
    QAudioBufferOutput tap;
    player.setAudioBufferOutput(&tap);
    player.setPlaybackRate(4.0);

    std::vector<qint64> times;
    std::vector<qint64> seeks;
    for (int i = 3; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        const qsizetype at = arg.indexOf('@');
        const qint64 t = (at < 0 ? arg : arg.left(at)).toLongLong();
        times.push_back(t);
        seeks.push_back(at < 0 ? qMax<qint64>(0, t - 400) : arg.mid(at + 1).toLongLong());
    }
    std::size_t index = 0;
    qint64 target = 0;
    qint64 seekMs = 0;
    const qint64 window = rate / 10;     // 100 ms
    const qint64 maxLag = rate * 6 / 100; // 60 ms
    std::vector<float> got;
    std::vector<bool> have;
    bool collecting = false;
    int result = 0;

    std::function<void()> next;
    const auto finish = [&] {
        collecting = false;
        player.pause();
        const qint64 base = target * rate / 1000;
        qint64 bestLag = 0;
        double best = 1e9;
        double atZero = 0;
        for (qint64 lag = -maxLag; lag <= maxLag; ++lag) {
            double sum = 0;
            int n = 0;
            for (qint64 i = 0; i < window; ++i) {
                if (have[static_cast<std::size_t>(i)]) {
                    sum += std::fabs(got[static_cast<std::size_t>(i)] - wavAt(base + i + lag));
                    ++n;
                }
            }
            const double mean = n ? sum / n : 1e9;
            if (lag == 0) {
                atZero = mean;
            }
            if (mean < best) {
                best = mean;
                bestLag = lag;
            }
        }
        double level = 0;
        float peak = 0;
        for (qint64 i = 0; i < window; ++i) {
            level += std::fabs(wavAt(base + i));
            peak = std::max(peak, std::fabs(wavAt(base + i)));
        }
        float gotPeak = 0;
        for (const float v : got) {
            gotPeak = std::max(gotPeak, std::fabs(v));
        }
        if (peak < 0.01F || gotPeak < 0.01F) {
            std::printf("T %lld ms (seek %lld): %s silent there, no lag to find\n", static_cast<long long>(target),
                        static_cast<long long>(seekMs), peak < 0.01F ? "the WAV is" : "the player's output is");
            QTimer::singleShot(50, &app, next);
            return;
        }
        std::printf("T %lld ms (seek %lld): best lag %lld samples (%.3f ms), mean difference %.6f there, %.6f at lag 0, "
                    "mean level %.4f\n", static_cast<long long>(target), static_cast<long long>(seekMs),
                    static_cast<long long>(bestLag),
                    bestLag * 1000.0 / rate, best, atZero, level / window);
        if (bestLag != 0) {
            result = 3;
        }
        QTimer::singleShot(50, &app, next);
    };
    QObject::connect(&tap, &QAudioBufferOutput::audioBufferReceived, &app, [&](const QAudioBuffer &buffer) {
        if (!collecting || !buffer.isValid()) {
            return;
        }
        if (buffer.format().sampleRate() != rate) {
            std::fprintf(stderr, "the player's buffers are at %d Hz, the WAV at %d Hz\n",
                         buffer.format().sampleRate(), rate);
            app.exit(1);
            return;
        }
        const qint64 first = std::llround(buffer.startTime() * static_cast<double>(rate) / 1e6) - target * rate / 1000;
        for (qsizetype i = 0; i < buffer.frameCount(); ++i) {
            const qint64 at = first + i;
            if (at >= 0 && at < window) {
                got[static_cast<std::size_t>(at)] = sampleAt(buffer, i);
                have[static_cast<std::size_t>(at)] = true;
            }
        }
        if (first + buffer.frameCount() >= window) {
            finish();
        }
    });
    next = [&] {
        if (index >= times.size()) {
            app.exit(result);
            return;
        }
        target = times[index];
        seekMs = seeks[index++];
        got.assign(static_cast<std::size_t>(window), 0.0F);
        have.assign(static_cast<std::size_t>(window), false);
        collecting = true;
        player.setPosition(seekMs);
        player.play();
    };
    QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &app, [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia && index == 0 && !collecting) {
            std::printf("%s: duration() %lld ms; WAV %lld frames at %d Hz (%.3f ms)\n", argv[1],
                        static_cast<long long>(player.duration()), static_cast<long long>(wavFrames), rate,
                        wavFrames * 1000.0 / rate);
            next();
        }
    });
    QObject::connect(&player, &QMediaPlayer::errorOccurred, &app, [&](QMediaPlayer::Error, const QString &message) {
        std::fprintf(stderr, "player error: %s\n", qPrintable(message));
        app.exit(1);
    });
    player.setSource(QUrl::fromLocalFile(QString::fromUtf8(argv[1])));
    QTimer::singleShot(30000 + 5000 * static_cast<int>(times.size()), &app, [&] {
        std::fprintf(stderr, "timed out\n");
        app.exit(1);
    });
    return app.exec();
}
