// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The real decoder lining up real files, written here rather than
// shipped (silence_probe_test's reasoning): one signal, and copies of it
// with a known number of samples of silence in front, a different tail,
// at another sample rate and in stereo, beside a different signal
// altogether. The shift measured must be the one put there, to within a
// sample or two, and every copy that does not line up must come back
// unknown.
//
// What this checks is the decoder's timeline, so the files are short:
// 6 s, under one window, so every window is the whole file. The rule
// over three different windows (a section cut from the middle, loops) is
// audio_alignment_test's, on signals made in memory. A CI runner can be
// thirty times slower per core than a desktop, and this has to fit its
// timeout there too.
//
// Run with --time <a> <b> to time one pair of real files instead.

#include <QCoreApplication>
#include <QTemporaryDir>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "infrastructure/audio/alignment_probe_factory.hpp"
#include "infrastructure/audio/identical_file_alignment_probe.hpp"
#include "infrastructure/audio/qt_multimedia_alignment_probe.hpp"
#include "infrastructure/paths/utf8_path.hpp"

using seabass::infrastructure::audio::IdenticalFileAlignmentProbe;
using seabass::infrastructure::audio::QtMultimediaAlignmentProbe;

namespace
{

// A signal defined at every instant, so the same music can be sampled at
// 44.1 kHz and at 48 kHz: tones at random frequencies, each swelling and
// fading at its own slow rate. Nothing in it repeats within a window.
std::function<double(double)> music(unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> frequency(80.0, 5000.0);
    std::uniform_real_distribution<double> swell(0.05, 1.5);
    std::uniform_real_distribution<double> phase(0.0, 2.0 * std::numbers::pi);
    struct Tone
    {
        double f, g, phi, psi;
    };
    std::vector<Tone> tones;
    for (int k = 0; k < 12; ++k) {
        tones.push_back({frequency(rng), swell(rng), phase(rng), phase(rng)});
    }
    return [tones](double t) {
        double sum = 0.0;
        for (const Tone &tone : tones) {
            const double amp = 0.5 + 0.5 * std::sin(2.0 * std::numbers::pi * tone.g * t + tone.psi);
            sum += amp * std::sin(2.0 * std::numbers::pi * tone.f * t + tone.phi);
        }
        return sum / 12.0;
    };
}

std::vector<double> sample(const std::function<double(double)> &signal, int rate, double seconds)
{
    std::vector<double> out(static_cast<size_t>(seconds * rate));
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = signal(static_cast<double>(i) / rate);
    }
    return out;
}

void appendLittleEndian(std::vector<char> &out, std::uint32_t value, int bytes)
{
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

// 16-bit PCM, every channel the same samples.
std::string writeWav(const std::string &path, const std::vector<double> &samples, int rate, int channels = 1)
{
    const auto frames = static_cast<std::uint32_t>(samples.size());
    const std::uint32_t dataBytes = frames * 2 * static_cast<std::uint32_t>(channels);
    std::vector<char> out;
    out.reserve(dataBytes + 44);
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    appendLittleEndian(out, 36 + dataBytes, 4);
    out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    appendLittleEndian(out, 16, 4);
    appendLittleEndian(out, 1, 2);
    appendLittleEndian(out, static_cast<std::uint32_t>(channels), 2);
    appendLittleEndian(out, static_cast<std::uint32_t>(rate), 4);
    appendLittleEndian(out, static_cast<std::uint32_t>(rate * 2 * channels), 4);
    appendLittleEndian(out, static_cast<std::uint32_t>(2 * channels), 2);
    appendLittleEndian(out, 16, 2);
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    appendLittleEndian(out, dataBytes, 4);
    for (double value : samples) {
        const auto s = static_cast<std::int16_t>(std::lround(std::clamp(value, -1.0, 1.0) * 30000.0));
        for (int c = 0; c < channels; ++c) {
            out.push_back(static_cast<char>(s & 0xFF));
            out.push_back(static_cast<char>((s >> 8) & 0xFF));
        }
    }
    std::ofstream file(seabass::pathFromUtf8(path), std::ios::binary);
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
    return path;
}

std::vector<double> withLead(size_t silentSamples, const std::vector<double> &samples)
{
    std::vector<double> out(silentSamples, 0.0);
    out.insert(out.end(), samples.begin(), samples.end());
    return out;
}

// Timed, so the cost of a pair is in the log.
std::optional<double> measure(QtMultimediaAlignmentProbe &probe, const std::string &a, const std::string &b,
                              const char *what)
{
    const auto start = std::chrono::steady_clock::now();
    const auto shift = probe.shiftMs(a, b);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    std::cout << "  " << what << ": " << (shift ? std::to_string(*shift) + " ms" : std::string("unknown")) << " in "
              << ms.count() << " ms\n";
    return shift;
}

bool near(double a, double b, double tolerance)
{
    return std::abs(a - b) <= tolerance;
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    if (argc == 4 && std::string(argv[1]) == "--time") {
        QtMultimediaAlignmentProbe probe;
        const auto start = std::chrono::steady_clock::now();
        const auto a = probe.decoded(argv[2]);
        const auto decodedA = std::chrono::steady_clock::now();
        const auto b = probe.decoded(argv[3]);
        const auto decodedB = std::chrono::steady_clock::now();
        const auto shift = probe.shiftMs(argv[2], argv[3]);
        const auto end = std::chrono::steady_clock::now();
        auto ms = [](auto from, auto to) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
        };
        std::cout << "decode a: " << ms(start, decodedA) << " ms, decode b: " << ms(decodedA, decodedB)
                  << " ms, correlate: " << ms(decodedB, end) << " ms, shift: "
                  << (shift ? std::to_string(*shift) + " ms" : std::string("unknown")) << "\n";
        return a && b ? 0 : 1;
    }

    assert(QtMultimediaAlignmentProbe::decodingAvailable() && "this build's decoder must load for this test to mean anything");

    QTemporaryDir dir;
    assert(dir.isValid());
    const std::string root = dir.path().toStdString();
    auto at = [&](const char *name) { return seabass::pathToUtf8(seabass::pathFromUtf8(root) / name); };

    constexpr int Rate = 44100;
    constexpr double Seconds = 6.0;
    const auto song = music(1);
    const std::vector<double> base = sample(song, Rate, Seconds);

    const std::string a = writeWav(at("a.wav"), base, Rate);
    // 1235 samples of silence in front: 28.005 ms, half a sample of the
    // analysis rate off any whole number of them.
    const std::string lead = writeWav(at("lead.wav"), withLead(1235, base), Rate);
    std::vector<double> tail = base;
    {
        const auto other = sample(music(2), Rate, 6.0);
        tail.insert(tail.end(), other.begin(), other.end());
    }
    const std::string longerTail = writeWav(at("tail.wav"), tail, Rate);
    const std::string different = writeWav(at("different.wav"), sample(music(3), Rate, Seconds), Rate);
    // The same music sampled at 48 kHz, in stereo, 250 ms later.
    const std::string at48k = writeWav(at("48k.wav"), withLead(12000, sample(song, 48000, Seconds)), 48000, 2);
    const std::string tiny = writeWav(at("tiny.wav"), std::vector<double>(base.begin(), base.begin() + Rate / 2), Rate);
    const std::string garbage = at("garbage.wav");
    std::ofstream(seabass::pathFromUtf8(garbage), std::ios::binary) << std::string(100000, 'x');

    QtMultimediaAlignmentProbe probe;
    const double tolerance = 2000.0 / 22050.0 * 1.001;  // two samples of the analysis rate, about 0.09 ms

    auto shift = measure(probe, a, lead, "silence in front");
    assert(shift && near(*shift, 1235.0 * 1000.0 / Rate, tolerance) && "leading silence is the shift");
    shift = measure(probe, lead, a, "the other way round");
    assert(shift && near(*shift, -1235.0 * 1000.0 / Rate, tolerance));
    shift = measure(probe, a, longerTail, "a different tail");
    assert(shift && near(*shift, 0.0, tolerance) && "a longer tail is no shift");
    shift = measure(probe, a, at48k, "48 kHz stereo, 250 ms later");
    assert(shift && near(*shift, 250.0, tolerance) && "another rate and channel count line up exactly");
    assert(!measure(probe, a, different, "a different signal") && "another song does not line up");
    assert(!measure(probe, a, tiny, "half a second") && "under a second is unknown");
    assert(!measure(probe, a, garbage, "not audio") && "an unreadable file is unknown");
    assert(!measure(probe, a, at("missing.wav"), "no file") && "a file that is not there is unknown");
    // The two kinds of unknown are kept apart: a cache keeps the first
    // and never the second.
    {
        using Kind = QtMultimediaAlignmentProbe::Answer::Kind;
        assert(probe.align(a, different).kind == Kind::NotLinedUp && "another song is decoded and does not line up");
        const auto unreadable = probe.align(a, garbage);
        assert(unreadable.kind == Kind::NoOpinion && !unreadable.unreadableA && unreadable.unreadableB
               && "a file that will not decode is no opinion, and says which");
        const auto missing = probe.align(at("missing.wav"), a);
        assert(missing.kind == Kind::NoOpinion && missing.unreadableA && !missing.unreadableB);
    }

    // A file in several pairs is decoded once while it is among the most
    // recently used, and what is kept is held to a number of bytes, not
    // of files.
    {
        const auto first = probe.decoded(a);
        const auto again = probe.decoded(a);
        assert(first && first == again && "the decoded file is reused");
        assert(probe.cachedBytes() <= (256u << 20));

        const size_t oneFile = first->bytes();
        // Room for three and a half of a's size; the file with the longer
        // tail weighs two.
        const size_t room = oneFile * 3 + oneFile / 2;
        QtMultimediaAlignmentProbe small(120000, room);
        const auto keptA = small.decoded(a);
        small.decoded(lead);
        assert(small.decoded(a) == keptA && "two files fit");
        small.decoded(longerTail);
        assert(small.cachedBytes() <= room && "the least recently used goes when the bytes run out");
        assert(small.decoded(a) == keptA && "the most recently used stays");
        assert(small.cachedBytes() == keptA->bytes() + small.decoded(longerTail)->bytes() && "that was lead");
        QtMultimediaAlignmentProbe tiny(120000, 1);
        assert(tiny.decoded(a) && tiny.cachedBytes() == oneFile && "one file is kept whatever it weighs");
        tiny.decoded(lead);
        assert(tiny.cachedBytes() == tiny.decoded(lead)->bytes() && "and only that one");
    }

    // Byte-identical copies need no decoder; anything else needs one.
    {
        std::filesystem::copy_file(seabass::pathFromUtf8(a), seabass::pathFromUtf8(at("copy.wav")));
        IdenticalFileAlignmentProbe bytesOnly(nullptr);
        shift = bytesOnly.shiftMs(a, at("copy.wav"));
        assert(shift && *shift == 0.0 && "the same bytes are lined up");
        assert(!bytesOnly.shiftMs(a, lead) && "without a decoder, anything else is unknown");
        assert(!bytesOnly.shiftMs(a, at("missing.wav")));
        assert(bytesOnly.identicalCount() == 1 && bytesOnly.measuredCount() == 0 && bytesOnly.unknownCount() == 2);
        // Same size, one byte different: not identical.
        std::vector<double> nudged = base;
        nudged[Rate * 3] += 0.01;
        const std::string almost = writeWav(at("almost.wav"), nudged, Rate);
        assert(std::filesystem::file_size(seabass::pathFromUtf8(almost)) == std::filesystem::file_size(seabass::pathFromUtf8(a)));
        assert(!seabass::infrastructure::audio::filesHaveSameBytes(a, almost) && "equal sizes are not equal bytes");

        auto everyBuild = seabass::infrastructure::audio::makeAudioAlignmentProbe({});
        assert(everyBuild->hasDecoder());
        shift = everyBuild->shiftMs(a, almost);
        assert(shift && near(*shift, 0.0, tolerance) && "a near copy is decoded and lined up");
        assert(everyBuild->linedUpCount() == 1 && everyBuild->unknownCount() == 0);
    }

    std::cout << "alignment_probe_test: all assertions passed\n";
    return 0;
}
