// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The scrolling waveform an Engine track's zoomed popup draws, made from
// the audio (infrastructure/audio/qt_multimedia_detail_waveform.hpp), on
// the beat-click files in tests/fixtures/beat-click (a click every 500 ms
// from 1.000 s, tools/mp3-offset-probe/make-beat-click-mp3s.py with 10
// seconds) and a WAV of the same audio this test writes.
//
// On Engine's timeline the click encoded at T seconds is at sample
//
//     T * rate + 576 + 529 + samplesBeforeAudio(leadIn, EngineFfmpegGapless)
//
// (qt_decoder_timeline_test's arithmetic: libmp3lame's delay, the MP3
// decoder's own, and what Engine's FFmpeg trims or adds), and at T * rate
// in the WAV. For every file:
//   * the decoder hands over the 1.000 s click within 2 samples of there
//     (the first click is the loudest sample of the first two seconds);
//   * the column holding the 9.000 s click's attack (the first column
//     after 8.9 s whose loudest band passes 0.1) is the column that sample
//     falls in: exactly for the WAV, give or take one for an MP3 (see
//     checkFile());
//   * between that click and the next the columns are near silent;
//   * 150 columns a second.
//
//   audio_detail_waveform_test FIXTURE_DIR
//
// No decoding backend is a FAIL, not a skip (docs/testing.md).

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "domain/mp3_lead_in.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/audio/qt_multimedia_detail_waveform.hpp"

using seabass::domain::Mp3Decoder;
using seabass::domain::samplesBeforeAudio;
namespace audio = seabass::infrastructure::audio;

namespace
{

constexpr int LibMp3LameDelay = 576;
constexpr int DecoderDelay = 529;
constexpr long ToleranceSamples = 2;
constexpr double ColumnsPerSecond = 150.0;

int failures = 0;

void check(bool ok, const std::string &what)
{
    std::cout << (ok ? "PASS " : "FAIL ") << what << "\n";
    if (!ok) {
        ++failures;
    }
}

// make-beat-click-mp3s.py's audio: a 2 ms burst of 2 kHz from 1.000 s
// every 500 ms, every fourth at full scale and the others at half,
// stereo 16-bit.
bool writeBeatClickWav(const QString &path, int rate, int seconds)
{
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(seconds) * rate, 0);
    const int burst = static_cast<int>(0.002 * rate);
    for (int beat = 0;; ++beat) {
        const long at = std::lround((1.0 + beat * 0.5) * rate);
        if (at + burst >= static_cast<long>(pcm.size())) {
            break;
        }
        const double level = beat % 4 == 0 ? 1.0 : 0.5;
        for (int i = 0; i < burst; ++i) {
            const double decay = std::exp(-6.0 * i / burst);
            pcm[static_cast<std::size_t>(at + i)] = static_cast<std::int16_t>(
                32767 * level * decay * std::cos(2 * 3.141592653589793 * 2000.0 * i / rate));
        }
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    const auto u32 = [&](std::uint32_t v) {
        const char b[4] = {char(v & 0xff), char((v >> 8) & 0xff), char((v >> 16) & 0xff), char((v >> 24) & 0xff)};
        file.write(b, 4);
    };
    const auto u16 = [&](std::uint16_t v) {
        const char b[2] = {char(v & 0xff), char((v >> 8) & 0xff)};
        file.write(b, 2);
    };
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(pcm.size() * 4);
    file.write("RIFF", 4);
    u32(36 + dataBytes);
    file.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(static_cast<std::uint32_t>(rate));
    u32(static_cast<std::uint32_t>(rate * 4));
    u16(4);
    u16(16);
    file.write("data", 4);
    u32(dataBytes);
    for (const std::int16_t v : pcm) {
        u16(static_cast<std::uint16_t>(v));
        u16(static_cast<std::uint16_t>(v));
    }
    return file.error() == QFile::NoError;
}

// `attackTolerance`: 0 for the WAV, whose click starts exactly where it
// was written. 1 for an MP3: the codec smears a click's energy a little
// ahead of it (pre-echo), and where the click falls right at the start of
// a column, as at 9.000 s on Engine's timeline when the encoder delay is
// trimmed, that is enough to light the column before it.
void checkFile(const std::string &path, const std::string &name, long leadSamples, long attackTolerance)
{
    // The decoder's own samples: where the 1.000 s click lands.
    std::vector<float> mono;
    int rate = 0;
    const bool decoded = audio::decodeMono(path, [&](const float *samples, std::size_t count, int sampleRate) {
        rate = sampleRate;
        mono.insert(mono.end(), samples, samples + count);
    });
    if (!decoded || rate <= 0) {
        check(false, name + ": decoded");
        return;
    }
    const std::size_t window = std::min(mono.size(), static_cast<std::size_t>(2 * rate));
    long first = -1;
    float peak = 0.0F;
    for (std::size_t i = 0; i < window; ++i) {
        if (std::fabs(mono[i]) > peak) {
            peak = std::fabs(mono[i]);
            first = static_cast<long>(i);
        }
    }
    const long expectedFirst = rate + leadSamples;
    check(std::labs(first - expectedFirst) <= ToleranceSamples,
          name + ": the 1.000 s click at sample " + std::to_string(first) + " (" + std::to_string(first * 1000.0 / rate)
              + " ms), Engine's timeline puts it at " + std::to_string(expectedFirst));

    const seabass::domain::DetailWaveform detail = audio::audioDetailWaveform(path);
    check(std::abs(detail.msPerColumn - 1000.0 / ColumnsPerSecond) < 1e-9, name + ": 150 columns a second");
    const double seconds = static_cast<double>(mono.size()) / rate;
    check(std::abs(static_cast<double>(detail.columns.size()) - seconds * ColumnsPerSecond) <= 1.0,
          name + ": " + std::to_string(detail.columns.size()) + " columns for " + std::to_string(seconds) + " s");
    const auto loudest = [&](std::size_t c) {
        const auto &col = detail.columns[c];
        return std::max({col.low, col.mid, col.high});
    };
    const long clickSample = 9L * rate + leadSamples;
    const long expectedColumn = static_cast<long>(clickSample * ColumnsPerSecond / rate);
    long attack = -1;
    for (std::size_t c = static_cast<std::size_t>(8.9 * ColumnsPerSecond); c < detail.columns.size(); ++c) {
        if (loudest(c) > 0.1) {
            attack = static_cast<long>(c);
            break;
        }
    }
    check(std::labs(attack - expectedColumn) <= attackTolerance,
          name + ": the 9.000 s click's attack in column " + std::to_string(attack) + ", expected "
              + std::to_string(expectedColumn));
    double between = 0.0;
    const double clickSeconds = static_cast<double>(clickSample) / rate;
    for (auto c = static_cast<std::size_t>((clickSeconds + 0.1) * ColumnsPerSecond);
         c < static_cast<std::size_t>((clickSeconds + 0.45) * ColumnsPerSecond) && c < detail.columns.size(); ++c) {
        between = std::max(between, loudest(c));
    }
    check(between < 0.02, name + ": silence between clicks, loudest band " + std::to_string(between));
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::cerr << "usage: audio_detail_waveform_test FIXTURE_DIR\n";
        return 2;
    }
    const std::string dir = std::string(argv[1]) + "/";
    const char *const files[] = {
        "beats-A-notag-44100.mp3", "beats-B-noencoder-44100.mp3", "beats-B-lavc-44100.mp3",
        "beats-C-badcrc-44100.mp3", "beats-D-lame-44100.mp3",     "beats-D-lame-48000.mp3",
    };
    for (const char *file : files) {
        const std::string path = dir + file;
        const auto leadIn = audio::readMp3LeadIn(path);
        if (!leadIn) {
            check(false, std::string(file) + ": lead-in read");
            continue;
        }
        checkFile(path, file,
                  LibMp3LameDelay + DecoderDelay + samplesBeforeAudio(*leadIn, Mp3Decoder::EngineFfmpegGapless), 1);
    }

    QTemporaryDir temp;
    const QString wav = temp.filePath(QStringLiteral("beats-ref-44100.wav"));
    if (!temp.isValid() || !writeBeatClickWav(wav, 44100, 10)) {
        check(false, "the WAV written");
    } else {
        checkFile(wav.toStdString(), "beats-ref-44100.wav", 0, 0);
    }

    check(audio::audioDetailWaveform(dir + "no-such-file.mp3").columns.empty(), "a missing file gives no columns");

    if (failures > 0) {
        std::cout << failures << " checks FAILED\n";
        return 1;
    }
    std::cout << "audio_detail_waveform_test: all passed\n";
    return 0;
}
