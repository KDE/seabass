// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The real decoder against real audio, written here rather than shipped:
// a fixture of two audio files would have to be committed, licensed and
// trusted, where a generated tone is exactly as long and exactly as loud
// as this test says it is.
//
// The point being proved is not "QAudioDecoder works". It is that the
// number this probe hands to a destructive caller is the number a person
// would measure by looking at the waveform: where the music starts, and
// where it stops.

#include <QCoreApplication>
#include <QTemporaryDir>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "infrastructure/audio/qt_multimedia_silence_probe.hpp"

using seabass::infrastructure::audio::QtMultimediaSilenceProbe;

namespace
{

constexpr int SampleRate = 44100;

void appendLittleEndian(std::vector<char> &out, std::uint32_t value, int bytes)
{
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

// A mono 16-bit PCM WAV: `leadSeconds` of digital silence, `toneSeconds`
// of a full-scale 440 Hz sine, then `trailSeconds` of silence again.
std::string writeWav(const std::string &path, double leadSeconds, double toneSeconds, double trailSeconds)
{
    const auto leadFrames = static_cast<std::uint32_t>(leadSeconds * SampleRate);
    const auto toneFrames = static_cast<std::uint32_t>(toneSeconds * SampleRate);
    const auto trailFrames = static_cast<std::uint32_t>(trailSeconds * SampleRate);
    const std::uint32_t frames = leadFrames + toneFrames + trailFrames;
    const std::uint32_t dataBytes = frames * 2;

    std::vector<char> out;
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    appendLittleEndian(out, 36 + dataBytes, 4);
    out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    appendLittleEndian(out, 16, 4);              // fmt chunk size
    appendLittleEndian(out, 1, 2);               // PCM
    appendLittleEndian(out, 1, 2);               // mono
    appendLittleEndian(out, SampleRate, 4);
    appendLittleEndian(out, SampleRate * 2, 4);  // byte rate
    appendLittleEndian(out, 2, 2);               // block align
    appendLittleEndian(out, 16, 2);              // bits per sample
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    appendLittleEndian(out, dataBytes, 4);

    for (std::uint32_t i = 0; i < frames; ++i) {
        std::int16_t sample = 0;
        if (i >= leadFrames && i < leadFrames + toneFrames) {
            const double t = static_cast<double>(i - leadFrames) / SampleRate;
            sample = static_cast<std::int16_t>(30000.0 * std::sin(2.0 * M_PI * 440.0 * t));
        }
        out.push_back(static_cast<char>(sample & 0xFF));
        out.push_back(static_cast<char>((sample >> 8) & 0xFF));
    }

    std::ofstream file(path, std::ios::binary);
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    return path;
}

bool near(double a, double b, double tolerance)
{
    return std::abs(a - b) <= tolerance;
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QTemporaryDir dir;
    assert(dir.isValid());
    const std::string plain = writeWav(dir.filePath("plain.wav").toStdString(), 0.0, 5.0, 0.0);
    const std::string padded = writeWav(dir.filePath("padded.wav").toStdString(), 2.0, 5.0, 3.0);
    const std::string longer = writeWav(dir.filePath("longer.wav").toStdString(), 0.0, 10.0, 0.0);
    const std::string silent = writeWav(dir.filePath("silent.wav").toStdString(), 4.0, 0.0, 0.0);

    QtMultimediaSilenceProbe probe;

    // Half a second either way. The decoder reports whole buffers and a
    // sine crosses zero 880 times a second, so the exact first sample
    // above the threshold is not the thing to pin; that the answer is
    // the right few seconds is.
    constexpr double Tolerance = 0.5;

    // The precondition, checked before anything is measured. This test
    // only exists where seabass_audio_qt was built, so a backend that
    // will not load here is a broken environment, not a configuration
    // to be tolerated: it FAILS rather than skipping.
    //
    // The project's rule, in its own words: a skip or a not-fully-run
    // check is a FAIL; plant the precondition or fail, never record it
    // green. An earlier version of this printed "SKIP" and returned 0,
    // which would have reported success on exactly the machine where
    // the one piece of coverage for the real decoder does nothing.
    if (!QtMultimediaSilenceProbe::decodingAvailable()) {
        std::cerr << "FAIL: no audio decoding backend (QAudioDecoder::isSupported() is false).\n"
                  << "      This test is only built where seabass_audio_qt is, so the FFmpeg\n"
                  << "      multimedia plugin is missing or did not load.\n";
        return 1;
    }

    auto measured = probe.measure(plain);
    if (!measured) {
        std::cerr << "FAIL: the backend reports it can decode, but a plain 5 s WAV measured nothing.\n";
        return 1;
    }

    // Case 1: a file that is music end to end has no silence at either
    // end, and its content is its whole length.
    {
        assert(near(measured->totalSeconds, 5.0, Tolerance));
        assert(near(measured->leadingSilenceSeconds, 0.0, Tolerance));
        assert(near(measured->trailingSilenceSeconds, 0.0, Tolerance));
        assert(near(measured->contentSeconds(), 5.0, Tolerance));
        std::cout << "case 1 (no padding, no silence found) OK\n";
    }

    // Case 2: the padding is found where it was put, and taken off.
    {
        auto got = probe.measure(padded);
        assert(got.has_value());
        assert(near(got->totalSeconds, 10.0, Tolerance));
        assert(near(got->leadingSilenceSeconds, 2.0, Tolerance));
        assert(near(got->trailingSilenceSeconds, 3.0, Tolerance));
        std::cout << "case 2 (2 s before and 3 s after, found) OK\n";
    }

    // Case 3: and this is the whole feature. Two files five seconds
    // apart in stored length hold the same five seconds of music, and
    // saying so is exactly what lets a DJ's two copies of one track be
    // recognised as two copies of one track.
    {
        auto bare = probe.measure(plain);
        auto wrapped = probe.measure(padded);
        assert(bare && wrapped);
        assert(std::abs(bare->totalSeconds - wrapped->totalSeconds) > 4.0 && "the files really do differ in length");
        assert(near(bare->contentSeconds(), wrapped->contentSeconds(), 1.0) && "the music does not");
        std::cout << "case 3 (padded and bare hold the same music) OK\n";
    }

    // Case 4: a genuinely longer recording is not made to agree by
    // trimming. Silence is taken off; music is not.
    {
        auto bare = probe.measure(plain);
        auto extended = probe.measure(longer);
        assert(bare && extended);
        assert(extended->contentSeconds() - bare->contentSeconds() > 4.0);
        std::cout << "case 4 (twice the music is still twice the music) OK\n";
    }

    // Case 5: a file that is silent end to end reports no content at
    // all, which is what stops every silent file on a stick being
    // called a copy of every other.
    {
        auto got = probe.measure(silent);
        assert(got.has_value());
        assert(got->contentSeconds() == 0.0);
        std::cout << "case 5 (a silent file has no content) OK\n";
    }

    // Case 6: a path that is not there is no opinion, not a zero.
    {
        assert(!probe.measure(dir.filePath("nope.wav").toStdString()).has_value());
        assert(!probe.measure("").has_value());
        std::cout << "case 6 (a missing file answers nothing) OK\n";
    }

    std::cout << "All silence_probe tests passed.\n";
    return 0;
}
