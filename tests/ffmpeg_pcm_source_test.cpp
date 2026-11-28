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

#include <algorithm>
#include <cmath>
#include <iostream>
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
    std::cout << label << ": " << got.size() << " samples at " << ours.sampleRate() << " Hz, " << differing
              << " differ (worst " << worst << "), damaged packets " << ours.damagedPackets() << "\n";
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
            dir.entryList({"*.mp3", "*.flac", "*.wav", "*.m4a"}, QDir::Files, QDir::Name);
        check(!files.isEmpty(), std::string("audio files in ") + argv[i]);
        for (const QString &name : files) {
            double qtMs = 0.0;
            double oursMs = 0.0;
            compare(dir.filePath(name).toStdString(), name.toStdString(), qtMs, oursMs);
            ++compared;
        }
    }
    // A file that is not there, and one that is not audio.
    FfmpegPcmSource missing(std::string(argv[1]) + "/no-such-file.mp3");
    std::vector<float> chunk(64);
    check(!missing.error().empty() && missing.sampleRate() == 0 && missing.read(chunk) == 0, "a missing file is an error");
    FfmpegPcmSource notAudio(std::string(argv[1]) + "/README.md");
    check(!notAudio.error().empty() && notAudio.read(chunk) == 0, "a file that is not audio is an error");

    if (failures > 0) {
        std::cerr << failures << " check(s) FAILED over " << compared << " files\n";
        return 1;
    }
    std::cout << "ffmpeg_pcm_source_test: " << compared << " files, all the same through both decoders\n";
    return 0;
}
