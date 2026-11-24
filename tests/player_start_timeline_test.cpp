// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// #109: the position Qt Multimedia's player gives the samples it plays,
// against the decoder's timeline, from the start of a file as well as
// after a seek. The player is the app's (QMediaPlayer, FFmpeg backend) with
// the guard the app's PlaybackController puts on it
// (infrastructure/audio/player_start_guard.hpp); a QAudioBufferOutput, and
// no sound device, hands over every buffer it plays with the position it
// plays it at. For each file, a click is found by its loudest sample in a
// window, once in what QAudioDecoder decodes (the timeline of
// qt_decoder_timeline_test, which the cue and beat shifts are made for)
// and once in what the player plays, and the two positions must agree:
//
//   start    loaded and played, as PlaybackController::load() does
//   zero     a seek to 0 (a cue at 0:00, back to the start) then play
//   seek     a seek 300 to 400 ms before a later click, then play
//   replay   (one file) played to its end, then play() again
//   stopped  (one file) played, stopped, played again: the same file
//            loaded again, which does not load anew
//
// Without the guard a LAME or Lavc file is 25.06 ms off from the start (23
// at 48 kHz, 24 for MPEG-2) and agrees after a seek. The player places
// samples exactly, so the tolerance is 1 ms: one MPEG frame (26 ms at 44.1
// kHz, the buffer the player hands over) would not tell the two apart.
// WAV and M4A have nothing to guard and must agree as they are.
//
//   player_start_timeline_test BEAT_CLICK_DIR MP3_CLICK_DIR ALIGNMENT_DIR
//
// No decoding backend is a FAIL, not a skip (docs/testing.md).

#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QAudioDecoder>
#include <QCoreApplication>
#include <QDataStream>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "infrastructure/audio/player_start_guard.hpp"

using seabass::infrastructure::audio::PlayerStartGuard;
using seabass::infrastructure::audio::playerStartMs;

namespace
{

constexpr double ToleranceMs = 1.0;

float sampleAt(const QAudioBuffer &buffer, qsizetype frame)
{
    const QAudioFormat format = buffer.format();
    const qsizetype at = frame * format.channelCount();
    switch (format.sampleFormat()) {
    case QAudioFormat::Float:
        return buffer.constData<float>()[at];
    case QAudioFormat::Int16:
        return buffer.constData<qint16>()[at] / 32768.0F;
    case QAudioFormat::Int32:
        return static_cast<float>(buffer.constData<qint32>()[at] / 2147483648.0);
    case QAudioFormat::UInt8:
        return (buffer.constData<quint8>()[at] - 128) / 128.0F;
    default:
        return 0.0F;
    }
}

// The loudest sample of the first channel between two times, by the
// time each sample is given: in ms, or a negative number for none.
struct Loudest
{
    double atMs = -1.0;
    float magnitude = 0.0F;
};

struct Decoded
{
    std::vector<float> samples;
    int rate = 0;
    std::string error;
};

Decoded decode(const QString &path)
{
    Decoded out;
    QAudioDecoder decoder;
    if (!decoder.isSupported()) {
        out.error = "no audio decoding backend";
        return out;
    }
    decoder.setSource(QUrl::fromLocalFile(path));
    QEventLoop loop;
    QObject::connect(&decoder, &QAudioDecoder::bufferReady, &loop, [&]() {
        while (decoder.bufferAvailable()) {
            const QAudioBuffer buffer = decoder.read();
            if (!buffer.isValid()) {
                continue;
            }
            out.rate = buffer.format().sampleRate();
            for (qsizetype i = 0; i < buffer.frameCount(); ++i) {
                out.samples.push_back(sampleAt(buffer, i));
            }
        }
    });
    QObject::connect(&decoder, &QAudioDecoder::finished, &loop, &QEventLoop::quit);
    QObject::connect(&decoder, QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error), &loop,
                     [&](QAudioDecoder::Error) {
                         out.error = decoder.errorString().toStdString();
                         loop.quit();
                     });
    QTimer::singleShot(30000, &loop, [&]() {
        out.error = "decoding timed out";
        loop.quit();
    });
    decoder.start();
    loop.exec();
    return out;
}

Loudest loudestDecoded(const Decoded &decoded, double fromMs, double toMs)
{
    Loudest loudest;
    const auto from = static_cast<std::size_t>(std::max(0.0, fromMs * decoded.rate / 1000.0));
    const auto to = std::min(decoded.samples.size(), static_cast<std::size_t>(toMs * decoded.rate / 1000.0));
    for (std::size_t i = from; i < to; ++i) {
        if (std::fabs(decoded.samples[i]) > loudest.magnitude) {
            loudest.magnitude = std::fabs(decoded.samples[i]);
            loudest.atMs = static_cast<double>(i) * 1000.0 / decoded.rate;
        }
    }
    return loudest;
}

enum class Mode { Start, Zero, Seek, Replay, Stopped };

const char *name(Mode mode)
{
    switch (mode) {
    case Mode::Start: return "start";
    case Mode::Zero: return "zero";
    case Mode::Seek: return "seek";
    case Mode::Replay: return "replay";
    case Mode::Stopped: return "stopped";
    }
    return "?";
}

struct Played
{
    Loudest loudest;
    std::string error;
};

// Plays `path` as `mode` says and returns the loudest sample the player
// plays between fromMs and toMs, at the position it gives that sample.
Played play(const QString &path, Mode mode, qint64 seekMs, double fromMs, double toMs)
{
    Played out;
    QMediaPlayer player;
    QAudioBufferOutput tap;
    player.setAudioBufferOutput(&tap);
    // Made before anything else connects to the player, as in the app.
    PlayerStartGuard guard(&player);
    QEventLoop loop;
    bool listening = mode != Mode::Replay && mode != Mode::Stopped;
    bool done = false;
    bool started = mode == Mode::Start;

    QObject::connect(&tap, &QAudioBufferOutput::audioBufferReceived, &loop, [&](const QAudioBuffer &buffer) {
        if (!listening || done || !buffer.isValid()) {
            return;
        }
        const double rate = buffer.format().sampleRate();
        const double startMs = buffer.startTime() / 1000.0;
        for (qsizetype i = 0; i < buffer.frameCount(); ++i) {
            const double atMs = startMs + i * 1000.0 / rate;
            const float magnitude = std::fabs(sampleAt(buffer, i));
            if (atMs >= fromMs && atMs < toMs && magnitude > out.loudest.magnitude) {
                out.loudest.magnitude = magnitude;
                out.loudest.atMs = atMs;
            }
        }
        if (startMs > toMs) {
            done = true;
            loop.quit();
        }
    });
    QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &loop, [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia && !started) {
            started = true;
            guard.seek(mode == Mode::Zero ? 0 : seekMs);
            player.play();
            if (mode == Mode::Stopped) {
                QTimer::singleShot(300, &loop, [&]() {
                    player.stop();
                    listening = true;
                    guard.play();
                });
            }
        } else if (status == QMediaPlayer::EndOfMedia && mode == Mode::Replay && !listening) {
            listening = true;
            guard.play();
        }
    });
    QObject::connect(&player, &QMediaPlayer::errorOccurred, &loop, [&](QMediaPlayer::Error, const QString &message) {
        out.error = message.toStdString();
        loop.quit();
    });
    QTimer::singleShot(20000, &loop, [&]() {
        out.error = "timed out";
        loop.quit();
    });
    guard.setFile(path);
    player.setSource(QUrl::fromLocalFile(path));
    if (mode == Mode::Start) {
        // PlaybackController::load(): play() at once, before the file has
        // loaded.
        guard.play();
    }
    loop.exec();
    player.stop();
    return out;
}

// 4 s of 44.1 kHz stereo 16-bit silence with clicks at 1.0 and 2.5 s.
bool writeClickWav(const QString &path)
{
    constexpr int Rate = 44100;
    constexpr int Frames = Rate * 4;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    const auto tag = [&](const char *four) { out.writeRawData(four, 4); };
    tag("RIFF");
    out << quint32(36 + Frames * 4);
    tag("WAVE");
    tag("fmt ");
    out << quint32(16) << quint16(1) << quint16(2) << quint32(Rate) << quint32(Rate * 4) << quint16(4)
        << quint16(16);
    tag("data");
    out << quint32(Frames * 4);
    for (int i = 0; i < Frames; ++i) {
        const bool click = (i >= Rate && i < Rate + 20) || (i >= Rate * 5 / 2 && i < Rate * 5 / 2 + 20);
        const qint16 value = click ? static_cast<qint16>(i % 2 == 0 ? 24000 : -24000) : 0;
        out << value << value;
    }
    return out.status() == QDataStream::Ok;
}

struct Case
{
    QString path;
    double firstClickMs;   // where the file has its first click
    double laterClickMs;   // one that a seek reaches
    bool replay = false;
    bool guarded = true;   // a file whose start the guard moves
};

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 4) {
        std::cerr << "usage: player_start_timeline_test BEAT_CLICK_DIR MP3_CLICK_DIR ALIGNMENT_DIR\n";
        return 2;
    }
    const QString beat = QString::fromUtf8(argv[1]) + "/";
    const QString click = QString::fromUtf8(argv[2]) + "/";
    const QString alignment = QString::fromUtf8(argv[3]) + "/";
    QTemporaryDir scratch;
    const QString wav = scratch.filePath("click.wav");
    if (!scratch.isValid() || !writeClickWav(wav)) {
        std::cout << "FAIL could not write the click WAV\n";
        return 1;
    }

    // Beat-click files: a click every 500 ms from 1.0 s. Mp3-click files:
    // one click at 1.5 s. A tagless file has them one start offset later.
    const std::vector<Case> cases = {
        {beat + "beats-D-lame-44100.mp3", 1000, 2500},
        {beat + "beats-D-lame-48000.mp3", 1000, 2500},
        {beat + "beats-B-lavc-44100.mp3", 1000, 2500},
        {beat + "beats-C-badcrc-44100.mp3", 1000, 2500},
        {beat + "beats-B-noencoder-44100.mp3", 1025, 2525, false, false},
        {beat + "beats-A-notag-44100.mp3", 1025, 2525, false, false},
        {click + "click-D-lame-44100.mp3", 1500, 1500, true},
        {click + "click-D-lame-mpeg2-22050.mp3", 1500, 1500},
        {click + "click-D-lame-nocounts-44100.mp3", 1500, 1500},
        // A trim longer than a frame (1729 samples): the click 624
        // samples earlier, the cut frame 575 samples long.
        {click + "click-D-lame-delay1200-44100.mp3", 1486, 1486},
        {click + "click-D-l399-44100.mp3", 1525, 1525, false, false},
        {click + "click-VBRI-44100.mp3", 1525, 1525, false, false},
        {wav, 1000, 2500, false, false},
        {alignment + "a-ref.m4a", 840, 840, false, false},
    };

    int failures = 0;
    int checks = 0;
    for (const Case &c : cases) {
        const std::string file = QFileInfo(c.path).fileName().toStdString();
        const qint64 startMs = playerStartMs(c.path.toStdString());
        // The guard's own part: which files it moves at all.
        if ((startMs > 0) != c.guarded) {
            std::cout << "FAIL " << file << ": player start " << startMs << " ms, expected "
                      << (c.guarded ? "past 0" : "0") << "\n";
            ++failures;
        }
        const Decoded decoded = decode(c.path);
        if (!decoded.error.empty() || decoded.rate <= 0) {
            std::cout << "FAIL " << file << ": " << decoded.error << "\n";
            ++failures;
            continue;
        }
        std::vector<Mode> modes = {Mode::Start, Mode::Zero, Mode::Seek};
        if (c.replay) {
            modes.push_back(Mode::Replay);
            modes.push_back(Mode::Stopped);
        }
        for (const Mode mode : modes) {
            const double clickMs = mode == Mode::Seek ? c.laterClickMs : c.firstClickMs;
            const double fromMs = clickMs - 150;
            const double toMs = clickMs + 150;
            // A seek far enough ahead of the click for MP3's bit reservoir
            // (#92), and for replay one past the click, so the file ends.
            const qint64 seekMs = mode == Mode::Replay ? 2600 : static_cast<qint64>(clickMs - 400);
            // Stopped: played from before the click for a while first.
            const Loudest expected = loudestDecoded(decoded, fromMs, toMs);
            const Played played = play(c.path, mode, seekMs, fromMs, toMs);
            ++checks;
            if (!played.error.empty() || expected.atMs < 0 || played.loudest.atMs < 0
                || expected.magnitude < 0.1F) {
                std::cout << "FAIL " << file << " " << name(mode) << ": "
                          << (played.error.empty() ? "no click found" : played.error) << "\n";
                ++failures;
                continue;
            }
            const double offMs = played.loudest.atMs - expected.atMs;
            const bool pass = std::fabs(offMs) <= ToleranceMs;
            std::cout << (pass ? "PASS " : "FAIL ") << file << " " << name(mode) << ": decoded at "
                      << expected.atMs << " ms, played at " << played.loudest.atMs << " ms (" << offMs
                      << " ms), player start " << startMs << " ms\n";
            if (!pass) {
                ++failures;
            }
        }
    }
    if (failures > 0) {
        std::cout << failures << " FAILED of " << checks << " plays\n";
        return 1;
    }
    std::cout << "player_start_timeline_test: all " << checks << " plays agree\n";
    return 0;
}
