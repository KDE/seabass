// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A decoder that starts and then says nothing at all (a fault of Qt
// Multimedia's, qt_decoder_start.hpp) is started again by every class
// that decodes: the silence probe, the alignment probe and the detail
// waveform here, the PCM source in its own test. What comes after a
// silent start is what a plain decode gives; three silent starts are
// "could not be decoded"; a file the decoder refuses is not tried again.

#include <QCoreApplication>
#include <QElapsedTimer>

#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "infrastructure/audio/qt_decoder_start.hpp"
#include "infrastructure/audio/qt_multimedia_alignment_probe.hpp"
#include "infrastructure/audio/qt_multimedia_detail_waveform.hpp"
#include "infrastructure/audio/qt_multimedia_silence_probe.hpp"

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

// The attempts themselves: which are made, with which waits, and when
// they end.
void attempts()
{
    std::vector<decoder_start::Attempt> seen;
    int made = decoder_start::run("x", [&](const decoder_start::Attempt &attempt) {
        seen.push_back(attempt);
        return false;
    });
    check(made == 1 && seen.size() == 1 && seen[0].number == 1 && !seen[0].silenced
              && seen[0].firstSampleWaitMs == decoder_start::FirstSampleWaitMs,
          "a decoder that answers is started once, and waited for three seconds");

    seen.clear();
    made = decoder_start::run("x", [&](const decoder_start::Attempt &attempt) {
        seen.push_back(attempt);
        return true;
    });
    check(made == 3 && seen.size() == 3, "one that never answers is started three times and no more");
    check(seen.size() == 3 && seen[1].firstSampleWaitMs == decoder_start::FirstSampleWaitMs && seen[2].firstSampleWaitMs == 0,
          "the last attempt has no wait of its own, only the caller's timeout");

    seen.clear();
    decoder_start::setSilentAttemptsForTesting(2, 70);
    made = decoder_start::run("x", [&](const decoder_start::Attempt &attempt) {
        seen.push_back(attempt);
        return attempt.silenced;
    });
    decoder_start::setSilentAttemptsForTesting(0);
    check(made == 3 && seen.size() == 3 && seen[0].silenced && seen[1].silenced && !seen[2].silenced
              && seen[0].firstSampleWaitMs == 70 && seen[1].firstSampleWaitMs == 70,
          "a test's silent attempts are silenced, and waited for as long as it says");
}

// `decode` gives a file's decode as text (empty: could not be decoded),
// with the timeout it is handed for the whole file.
void startsAgain(const std::string &name, const std::string &file, const std::string &notAudio,
                 const std::function<std::string(const std::string &path, int timeoutMs)> &decode)
{
    const std::string expected = decode(file, 60000);
    check(!expected.empty(), name + ": the file decodes");

    for (int silent = 1; silent <= 2; ++silent) {
        decoder_start::setSilentAttemptsForTesting(silent, 100);
        QElapsedTimer timer;
        timer.start();
        const std::string got = decode(file, 60000);
        const qint64 took = timer.elapsed();
        decoder_start::setSilentAttemptsForTesting(0);
        check(got == expected, name + ": after " + std::to_string(silent) + " silent start(s) the same as a plain decode");
        // A coarse timer may ring a twentieth early.
        check(took >= 90 * silent, name + ": " + std::to_string(silent) + " silent start(s) were each waited for: "
                                       + std::to_string(took) + " ms");
    }

    // Three silent starts: given up, and the last one waited for the
    // whole of the caller's timeout (100 + 100 + 400).
    decoder_start::setSilentAttemptsForTesting(3, 100);
    QElapsedTimer timer;
    timer.start();
    const std::string never = decode(file, 400);
    const qint64 gaveUp = timer.elapsed();
    decoder_start::setSilentAttemptsForTesting(0);
    check(never.empty(), name + ": three silent starts are a file that could not be decoded");
    check(gaveUp >= 540, name + ": the last start has the caller's whole timeout: " + std::to_string(gaveUp) + " ms");

    // A file the decoder refuses says so at once: it is not waited for
    // three seconds twice over.
    timer.restart();
    check(decode(notAudio, 60000).empty(), name + ": a file that is not audio cannot be decoded");
    check(timer.elapsed() < 2500, name + ": and is refused at once: " + std::to_string(timer.elapsed()) + " ms");

    check(decode(file, 60000) == expected, name + ": the next decode is an ordinary one again");
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::cerr << "usage: qt_decoder_start_test ALIGNMENT_DIR\n";
        return 2;
    }
    if (!QtMultimediaSilenceProbe::decodingAvailable()) {
        std::cerr << "FAILED: no audio decoding backend\n";
        return 1;
    }
    const std::string dir = argv[1];
    const std::string file = dir + "/a-ref.flac";
    const std::string notAudio = dir + "/README.md";

    attempts();

    startsAgain("silence probe", file, notAudio, [](const std::string &path, int timeoutMs) {
        QtMultimediaSilenceProbe probe(-60.0, timeoutMs);
        const auto span = probe.measure(path);
        return span ? std::to_string(span->totalSeconds) + " " + std::to_string(span->leadingSilenceSeconds) + " "
                          + std::to_string(span->trailingSilenceSeconds)
                    : std::string();
    });

    startsAgain("alignment probe", file, notAudio, [](const std::string &path, int timeoutMs) {
        // A probe of its own each time: it keeps what it decoded.
        QtMultimediaAlignmentProbe probe(timeoutMs);
        const auto kept = probe.decoded(path);
        if (!kept) {
            return std::string();
        }
        double sum = 0.0;
        for (const auto &piece : kept->pieces) {
            for (const float sample : piece.samples) {
                sum += static_cast<double>(sample) * sample;
            }
        }
        return std::to_string(kept->total) + " " + std::to_string(kept->pieces.size()) + " " + std::to_string(sum);
    });

    startsAgain("detail waveform", file, notAudio, [](const std::string &path, int timeoutMs) {
        std::size_t count = 0;
        double sum = 0.0;
        int rate = 0;
        const bool decoded = decodeMono(
            path,
            [&](const float *samples, std::size_t n, int sampleRate) {
                rate = sampleRate;
                for (std::size_t i = 0; i < n; ++i) {
                    sum += static_cast<double>(samples[i]) * samples[i];
                }
                count += n;
            },
            {}, timeoutMs);
        return decoded ? std::to_string(rate) + " " + std::to_string(count) + " " + std::to_string(sum) : std::string();
    });

    if (failures > 0) {
        std::cerr << failures << " check(s) FAILED\n";
        return 1;
    }
    std::cout << "qt_decoder_start_test: all checks passed\n";
    return 0;
}
