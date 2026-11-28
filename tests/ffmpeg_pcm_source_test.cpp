// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The FFmpeg decoder of our own against the one the app uses: the same
// file must give the same samples through both, to the last one, or an
// analysis made through the one is not the analysis made through the
// other. Every audio file of the two fixture folders is compared.
//
// usage: ffmpeg_pcm_source_test DIR...
//        ffmpeg_pcm_source_test --files FILE...   (any files, with the time each decoder took)

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "infrastructure/audio/ffmpeg_pcm_source.hpp"
#include "infrastructure/audio/qt_multimedia_pcm_source.hpp"

using namespace seabass::infrastructure::audio;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    if (!ok) {
        std::cerr << "FAILED: " << what << "\n";
        ++failures;
    }
}

std::vector<float> all(seabass::domain::PcmSource &source)
{
    std::vector<float> out;
    std::vector<float> chunk(8192);
    for (;;) {
        const std::size_t got = source.read(chunk);
        if (got == 0) {
            break;
        }
        out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return out;
}

// One file through both decoders: no error, the same rate, the same
// number of samples and every sample the same.
void compare(const std::string &path, const std::string &label, double &qtMs, double &oursMs)
{
    QElapsedTimer timer;
    timer.start();
    QtMultimediaPcmSource qt(path);
    const std::vector<float> expected = all(qt);
    qtMs += static_cast<double>(timer.nsecsElapsed()) / 1e6;
    timer.restart();
    FfmpegPcmSource ours(path);
    const std::vector<float> got = all(ours);
    oursMs += static_cast<double>(timer.nsecsElapsed()) / 1e6;
    check(qt.error().empty(), label + ": the app's decoder reads it: " + qt.error());
    check(ours.error().empty(), label + ": ours reads it: " + ours.error());
    check(ours.sampleRate() == qt.sampleRate(),
          label + ": sample rate " + std::to_string(ours.sampleRate()) + " against " + std::to_string(qt.sampleRate()));
    check(got.size() == expected.size(),
          label + ": " + std::to_string(got.size()) + " samples against " + std::to_string(expected.size()));
    double worst = 0.0;
    std::size_t differing = 0;
    for (std::size_t s = 0; s < std::min(got.size(), expected.size()); ++s) {
        const double difference = std::abs(static_cast<double>(got[s]) - expected[s]);
        worst = std::max(worst, difference);
        differing += got[s] != expected[s];
    }
    check(differing == 0,
          label + ": " + std::to_string(differing) + " samples differ, the worst by " + std::to_string(worst));
    // No fixture is damaged audio: a packet left out would be a decoder
    // that reads a sound file wrongly.
    check(ours.damagedPackets() == 0, label + ": " + std::to_string(ours.damagedPackets()) + " packets refused as damaged");
    std::cout << label << ": " << got.size() << " samples at " << ours.sampleRate() << " Hz, " << differing
              << " differ (worst " << worst << "), damaged packets " << ours.damagedPackets() << "\n";
}

// A WAV of noise that the test writes, each channel its own: what the
// fixtures have little of. `format` 1 is integers of `bits` (8 unsigned,
// 16, 24, 32), 3 is 32-bit floats. The 32-bit ones matter most: such a
// sample does not fit a float, so a decoder that adds the channels
// before it rounds them gives, now and then, the float next to the one
// of a decoder that rounds first; the two must round alike.
bool writeNoiseWav(const QString &file, int format, int bits, int channels)
{
    const std::uint32_t frames = 22050;
    const std::uint32_t bytes = static_cast<std::uint32_t>(bits / 8);
    const std::uint32_t frameBytes = bytes * static_cast<std::uint32_t>(channels);
    QByteArray out;
    const auto put = [&out](std::uint32_t value, int count) {
        for (int i = 0; i < count; ++i) {
            out.append(static_cast<char>((value >> (8 * i)) & 0xFF));
        }
    };
    out.append("RIFF");
    put(36 + frames * frameBytes, 4);
    out.append("WAVEfmt ");
    put(16, 4);
    put(static_cast<std::uint32_t>(format), 2);
    put(static_cast<std::uint32_t>(channels), 2);
    put(44100, 4);
    put(44100 * frameBytes, 4);
    put(frameBytes, 2);
    put(static_cast<std::uint32_t>(bits), 2);
    out.append("data");
    put(frames * frameBytes, 4);
    std::mt19937 rng(static_cast<unsigned>(11 + bits + 100 * channels + format));
    std::uniform_real_distribution<float> level(-0.9f, 0.9f);
    for (std::uint32_t i = 0; i < frames * static_cast<std::uint32_t>(channels); ++i) {
        if (format == 3) {
            const float value = level(rng);
            std::uint32_t raw = 0;
            std::memcpy(&raw, &value, sizeof raw);
            put(raw, 4);
        } else {
            put(static_cast<std::uint32_t>(rng()), static_cast<int>(bytes));
        }
    }
    QFile wav(file);
    return wav.open(QIODevice::WriteOnly) && wav.write(out) == out.size();
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::cerr << "usage: ffmpeg_pcm_source_test DIR...\n";
        return 2;
    }
    if (!QtMultimediaPcmSource::decodingAvailable()) {
        std::cerr << "FAILED: no audio decoding backend to compare with\n";
        return 1;
    }
    int compared = 0;
    if (std::string(argv[1]) == "--files") {
        double qtMs = 0.0;
        double oursMs = 0.0;
        for (int i = 2; i < argc; ++i) {
            compare(argv[i], argv[i], qtMs, oursMs);
            ++compared;
        }
        std::cout << compared << " files, " << failures << " failed checks; the app's decoder " << qtMs / 1000.0
                  << " s, ours " << oursMs / 1000.0 << " s\n";
        return failures > 0 ? 1 : 0;
    }
    for (int i = 1; i < argc; ++i) {
        const QDir dir(QString::fromLocal8Bit(argv[i]));
        const QStringList files =
            dir.entryList({"*.mp3", "*.flac", "*.wav", "*.m4a", "*.ogg", "*.aiff"}, QDir::Files, QDir::Name);
        check(!files.isEmpty(), std::string("audio files in ") + argv[i]);
        for (const QString &name : files) {
            double qtMs = 0.0;
            double oursMs = 0.0;
            compare(dir.filePath(name).toStdString(), name.toStdString(), qtMs, oursMs);
            ++compared;
        }
    }
    {
        QTemporaryDir scratch;
        check(scratch.isValid(), "a scratch folder for the WAVs");
        struct Kind
        {
            const char *name;
            int format;
            int bits;
            int channels;
        };
        for (const Kind &kind : {Kind{"noise-8-bit-stereo.wav", 1, 8, 2}, Kind{"noise-16-bit-stereo.wav", 1, 16, 2},
                                 Kind{"noise-24-bit-stereo.wav", 1, 24, 2}, Kind{"noise-32-bit-stereo.wav", 1, 32, 2},
                                 Kind{"noise-float-stereo.wav", 3, 32, 2}, Kind{"noise-16-bit-mono.wav", 1, 16, 1},
                                 Kind{"noise-16-bit-three-channels.wav", 1, 16, 3},
                                 Kind{"noise-32-bit-six-channels.wav", 1, 32, 6}}) {
            const QString wav = scratch.filePath(kind.name);
            check(writeNoiseWav(wav, kind.format, kind.bits, kind.channels), std::string(kind.name) + " is written");
            double qtMs = 0.0;
            double oursMs = 0.0;
            compare(wav.toStdString(), kind.name, qtMs, oursMs);
            ++compared;
        }
    }

    // A file that is not there, and one that is not audio.
    FfmpegPcmSource missing(std::string(argv[1]) + "/no-such-file.mp3");
    std::vector<float> chunk(64);
    check(!missing.error().empty() && missing.sampleRate() == 0 && missing.read(chunk) == 0, "a missing file is an error");
    check(QFile::exists(QString::fromLocal8Bit(argv[1]) + "/README.md"), "the file that is not audio is there");
    FfmpegPcmSource notAudio(std::string(argv[1]) + "/README.md");
    check(!notAudio.error().empty() && notAudio.read(chunk) == 0, "a file that is not audio is an error");

    if (failures > 0) {
        std::cerr << failures << " check(s) FAILED over " << compared << " files\n";
        return 1;
    }
    std::cout << "ffmpeg_pcm_source_test: " << compared << " files, all the same through both decoders\n";
    return 0;
}
