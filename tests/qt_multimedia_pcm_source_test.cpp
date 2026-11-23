// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// QtMultimediaPcmSource, the decoder behind the PcmSource port, against
// files whose samples are known:
//
//   - the click MP3s (tests/fixtures/mp3-click): each click at the sample
//     qt_decoder_timeline_test expects of the player's decoder, so the
//     port's sample 0 is the player's (Engine's) timeline;
//   - the WAVs of tests/fixtures/alignment: a-lead160.wav is a-ref.wav
//     behind 160 ms of silence, and comes back as exactly that;
//   - a synthetic track written as a WAV comes back sample for sample at
//     the same positions;
//   - read in pieces, it holds a few buffers and never the track;
//   - a file that is not there or not audio is an error, at once.
//
//   qt_multimedia_pcm_source_test CLICK_DIR ALIGNMENT_DIR SCRATCH_DIR
//
// No decoding backend is a FAIL, not a skip (docs/testing.md).

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "domain/mp3_lead_in.hpp"
#include "domain/pcm_source.hpp"
#include "infrastructure/audio/mp3_lead_in_reader.hpp"
#include "infrastructure/audio/qt_multimedia_pcm_source.hpp"
#include "synth_track/synth_track.hpp"

using namespace seabass;
using infrastructure::audio::QtMultimediaPcmSource;

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

// Every sample, read in pieces of an awkward size so that reads straddle
// the decoder's buffers.
std::vector<float> readInPieces(domain::PcmSource &source, std::size_t piece = 1237)
{
    std::vector<float> all;
    std::vector<float> chunk(piece);
    for (;;) {
        const std::size_t got = source.read(chunk);
        if (got == 0) {
            break;
        }
        all.insert(all.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return all;
}

long loudestIn(const std::vector<float> &samples, long from, long to)
{
    long at = -1;
    float peak = 0.0F;
    for (long i = std::max(0L, from); i < std::min<long>(to, static_cast<long>(samples.size())); ++i) {
        if (std::fabs(samples[static_cast<std::size_t>(i)]) > peak) {
            peak = std::fabs(samples[static_cast<std::size_t>(i)]);
            at = i;
        }
    }
    return at;
}

// The expectation of qt_decoder_timeline_test, which measured the
// player's decoder on these files: 576 samples libmp3lame put in front,
// 529 of the decoder's own delay, less what the tag makes the decoder
// trim.
constexpr int LibMp3LameDelay = 576;
constexpr int DecoderDelay = 529;
constexpr long ToleranceSamples = 2;

void clickFiles(const std::string &dir)
{
    const char *const files[] = {
        "click-A-notag-44100.mp3",         "click-B-noencoder-44100.mp3",   "click-B-lavc-44100.mp3",
        "click-C-badcrc-44100.mp3",        "click-D-lame-44100.mp3",        "click-D-lame-48000.mp3",
        "click-D-lame-mono-44100.mp3",     "click-D-lame-mpeg2-22050.mp3",  "click-D-lame-crc-44100.mp3",
        "click-VBRI-44100.mp3",            "click-VBRI-modediffers-44100.mp3", "click-D-lame-junk-44100.mp3",
        "click-D-lame-id3junk-44100.mp3",  "click-D-lame-id3id3-44100.mp3", "click-D-lame-nocounts-44100.mp3",
        "click-VBRI-nocounts-44100.mp3",   "click-D-l399-44100.mp3",
    };
    for (const char *file : files) {
        const std::string path = dir + "/" + file;
        QtMultimediaPcmSource source(path);
        const std::vector<float> samples = readInPieces(source);
        if (!source.error().empty()) {
            check(false, std::string(file) + ": " + source.error());
            continue;
        }
        const auto leadIn = infrastructure::audio::readMp3LeadIn(path);
        if (!leadIn || leadIn->sampleRate != source.sampleRate()) {
            check(false, std::string(file) + ": lead-in unreadable or its rate is not the decoder's");
            continue;
        }
        const int rate = source.sampleRate();
        const long lead = LibMp3LameDelay + DecoderDelay
            + domain::samplesBeforeAudio(*leadIn, domain::Mp3Decoder::EngineFfmpegGapless);
        // The full-scale click at 1.5 s and the quarter one at 2.5 s.
        const long first = std::lround(1.5 * rate) + lead;
        const long second = std::lround(2.5 * rate) + lead;
        const long foundFirst = loudestIn(samples, 0, 2 * rate);
        const long foundSecond = loudestIn(samples, 2 * rate, 3 * rate);
        check(std::labs(foundFirst - first) <= ToleranceSamples,
              std::string(file) + ": click at " + std::to_string(foundFirst) + ", expected " + std::to_string(first));
        check(std::labs(foundSecond - second) <= ToleranceSamples,
              std::string(file) + ": second click at " + std::to_string(foundSecond) + ", expected "
                  + std::to_string(second));
        check(std::fabs(samples[static_cast<std::size_t>(std::max(0L, foundFirst))]) > 0.25F,
              std::string(file) + ": the click is loud");
    }
}

void alignmentWavs(const std::string &dir)
{
    QtMultimediaPcmSource refSource(dir + "/a-ref.wav");
    QtMultimediaPcmSource leadSource(dir + "/a-lead160.wav");
    const auto ref = readInPieces(refSource, 4096);
    const auto lead = readInPieces(leadSource, 333);
    check(refSource.error().empty() && leadSource.error().empty(),
          "alignment WAVs decode: " + refSource.error() + leadSource.error());
    check(refSource.sampleRate() == 11025 && leadSource.sampleRate() == 11025, "alignment WAVs: 11.025 kHz");
    // The data chunks hold 49612 and 51376 samples.
    check(ref.size() == 49612, "a-ref.wav: " + std::to_string(ref.size()) + " samples");
    check(lead.size() == 51376, "a-lead160.wav: " + std::to_string(lead.size()) + " samples");
    constexpr std::size_t Silence = 1764;  // 160 ms at 11.025 kHz
    bool silent = lead.size() > Silence;
    for (std::size_t i = 0; silent && i < Silence; ++i) {
        silent = lead[i] == 0.0F;
    }
    check(silent, "a-lead160.wav: 160 ms of silence in front");
    std::size_t differing = 0;
    for (std::size_t i = 0; i < ref.size() && i + Silence < lead.size(); ++i) {
        differing += ref[i] != lead[i + Silence] ? 1 : 0;
    }
    check(differing == 0, "a-lead160.wav is a-ref.wav 1764 samples on: " + std::to_string(differing) + " differ");
    double energy = 0.0;
    for (const float s : ref) {
        energy += static_cast<double>(s) * s;
    }
    check(energy > 1.0, "a-ref.wav is not silence");
}

void synthRoundTrip(const std::string &scratch, const std::string &presetName)
{
    auto spec = *synth::synthPreset(presetName);
    // Four bars are enough and keep the file small.
    spec.sections = {{"all", 4, true, true, true, true, true, false}};
    spec.tailMs = 500.0;
    const synth::SynthTrack track = synth::renderSynthTrack(spec);
    const std::string path = scratch + "/" + presetName + ".wav";
    std::string error;
    check(synth::writeWav16(path, track.samples, track.sampleRate, &error), presetName + ": WAV written " + error);

    QtMultimediaPcmSource source(path);
    const auto decoded = readInPieces(source);
    check(source.error().empty(), presetName + ": WAV decodes: " + source.error());
    check(source.sampleRate() == track.sampleRate, presetName + ": rate " + std::to_string(source.sampleRate()));
    check(decoded.size() == track.samples.size(),
          presetName + ": " + std::to_string(decoded.size()) + " samples back of " + std::to_string(track.samples.size()));
    // 16 bits: half a step of rounding, and the decoder's divisor
    // (32768 against the writer's 32767).
    double worst = 0.0;
    for (std::size_t i = 0; i < std::min(decoded.size(), track.samples.size()); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(decoded[i]) - track.samples[i]));
    }
    check(worst < 1e-4, presetName + ": every sample back where it was, worst difference " + std::to_string(worst));
}

void streaming(const std::string &dir)
{
    // 85 s at 11.025 kHz: 937000 samples, read a little at a time.
    QtMultimediaPcmSource source(dir + "/L-ref.flac");
    std::size_t total = 0;
    std::vector<float> chunk(512);
    for (;;) {
        const std::size_t got = source.read(chunk);
        if (got == 0) {
            break;
        }
        total += got;
    }
    check(source.error().empty(), "L-ref.flac decodes: " + source.error());
    check(total == 937125, "L-ref.flac: " + std::to_string(total) + " samples");
    // A second of audio at the most; the whole track would be 937125.
    check(source.peakHeldSamples() > 0 && source.peakHeldSamples() <= 11025,
          "streaming holds " + std::to_string(source.peakHeldSamples()) + " samples at most");
    std::cout << "L-ref.flac: " << total << " samples, at most " << source.peakHeldSamples() << " held\n";
}

void errors(const std::string &dir)
{
    QElapsedTimer timer;
    timer.start();
    QtMultimediaPcmSource missing(dir + "/no-such-file.mp3");
    std::vector<float> chunk(64);
    check(!missing.error().empty() && missing.sampleRate() == 0 && missing.read(chunk) == 0,
          "a missing file is an error");
    QtMultimediaPcmSource notAudio(dir + "/README.md");
    check(!notAudio.error().empty() && notAudio.read(chunk) == 0, "a file that is not audio is an error");
    check(timer.elapsed() < 5000,
          "errors come at once, not after the stall timeout: " + std::to_string(timer.elapsed()) + " ms");
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 4) {
        std::cerr << "usage: qt_multimedia_pcm_source_test CLICK_DIR ALIGNMENT_DIR SCRATCH_DIR\n";
        return 2;
    }
    if (!QtMultimediaPcmSource::decodingAvailable()) {
        std::cerr << "FAILED: no audio decoding backend\n";
        return 1;
    }
    QDir().mkpath(QString::fromLocal8Bit(argv[3]));
    QTemporaryDir scratch(QString::fromLocal8Bit(argv[3]) + "/pcm-source-XXXXXX");
    if (!scratch.isValid()) {
        std::cerr << "FAILED: no scratch directory\n";
        return 1;
    }
    clickFiles(argv[1]);
    alignmentWavs(argv[2]);
    synthRoundTrip(scratch.path().toStdString(), "house-124");
    synthRoundTrip(scratch.path().toStdString(), "house-124-48k");
    streaming(argv[2]);
    errors(argv[2]);
    if (failures > 0) {
        std::cerr << failures << " check(s) FAILED\n";
        return 1;
    }
    std::cout << "qt_multimedia_pcm_source_test: all checks passed\n";
    return 0;
}
