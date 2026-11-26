// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// QtFlacRecordingEncoder, keeping a recording as FLAC, against WAVs this
// test writes from a known signal (tones under noise that reaches every
// bit):
//
//   - 16 bit mono and stereo at 44.1, 48, 88.2 and 96 kHz, and 24 bit,
//     come out as a FLAC that decodes, through a decoder of the test's
//     own, to the signal, sample for sample, every channel, no frame more
//     or less; WAVE_FORMAT_EXTENSIBLE and RF64 headers likewise;
//   - progress runs from 0 to 1 and never back, and the encoder works
//     from a worker thread;
//   - a WAV cut short, not a WAV, another sample format, an unfinished
//     header: refused, nothing written;
//   - a target that exists: refused, left as it was; a folder that is a
//     file, a folder in the way of the temporary file: refused;
//   - a FLAC damaged after the encoding (cut, or bytes changed): the check
//     refuses it, nothing under the target's name;
//   - a cancel while encoding and while checking: no temporary file, no
//     target.
//
//   recording_encoder_test SCRATCH_DIR [--long [MINUTES]]
//
// With --long it encodes one WAV of several minutes (10, or MINUTES) and
// checks that the process's peak memory grew by much less than the WAV
// (the WAV is read and checked a piece at a time), and prints the time
// the encoding took.
//
// No encoding backend is a FAIL, not a skip (docs/testing.md).

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#include "infrastructure/audio/qt_flac_recording_encoder.hpp"
#include "infrastructure/audio/wav_pcm_file.hpp"
#include "recording_test_support.hpp"

using namespace seabass;
using infrastructure::audio::QtFlacRecordingEncoder;
using infrastructure::audio::RecordingEncodeTempSuffix;
using recording_test::WavSpec;
namespace fs = std::filesystem;

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

bool exists(const std::string &path)
{
    std::error_code ec;
    return fs::exists(pathFromUtf8(path), ec);
}

std::string readAll(const std::string &path)
{
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Nothing of the encoder's is left beside `flac`.
bool noTempBeside(const std::string &flac)
{
    return !exists(flac + RecordingEncodeTempSuffix);
}

struct Run
{
    application::RecordingEncodeResult result;
    std::vector<double> progress;
};

Run encode(QtFlacRecordingEncoder &encoder, const std::string &wav, const std::string &flac,
           application::CancellationToken cancel = application::CancellationToken::none(),
           double cancelAt = 2.0)
{
    Run run;
    application::RecordingEncodeOptions options;
    options.cancel = cancel;
    options.progress = [&run, cancel, cancelAt](double fraction) mutable {
        run.progress.push_back(fraction);
        if (fraction >= cancelAt) {
            cancel.cancel();
        }
    };
    run.result = encoder.encode(wav, flac, options);
    return run;
}

std::string name(const WavSpec &spec)
{
    return std::to_string(spec.bits) + "bit-" + std::to_string(spec.channels) + "ch-" + std::to_string(spec.rate) + "-"
        + std::to_string(spec.frames)
        + (spec.extensible ? "-ext" : "") + (spec.rf64 ? "-rf64" : "") + (spec.extraChunk ? "-list" : "");
}

// A WAV of `spec` encoded, then decoded by the test's own decoder and
// compared with the signal.
void roundTrip(const std::string &dir, const WavSpec &spec)
{
    const std::string label = name(spec);
    const std::string wav = dir + "/" + label + ".wav";
    const std::string flac = dir + "/" + label + ".flac";
    if (!recording_test::writeWav(wav, spec)) {
        check(false, label + ": the test could not write its WAV");
        return;
    }
    QtFlacRecordingEncoder encoder;
    const Run run = encode(encoder, wav, flac);
    check(run.result.encoded, label + ": encoded (" + run.result.error + ")");
    check(!run.result.cancelled && run.result.error.empty(), label + ": no error and no cancel beside encoded");
    check(run.result.framesInWav == spec.frames, label + ": the frames of the WAV are counted");
    check(run.result.framesVerified == spec.frames,
          label + ": every frame was checked (" + std::to_string(run.result.framesVerified) + ")");
    check(run.result.sampleRate == spec.rate && run.result.channels == spec.channels
              && run.result.bitsPerSample == spec.bits,
          label + ": the WAV's format is reported");
    check(noTempBeside(flac), label + ": no temporary file is left");
    const std::string mismatch = recording_test::compareWithSignal(recording_test::decodeAll(flac), spec);
    check(mismatch.empty(), label + ": the FLAC decodes to the signal: " + mismatch);

    bool monotonic = true;
    for (std::size_t i = 1; i < run.progress.size(); ++i) {
        monotonic = monotonic && run.progress[i] >= run.progress[i - 1];
    }
    check(!run.progress.empty() && monotonic && run.progress.front() >= 0.0 && run.progress.back() == 1.0,
          label + ": progress runs from 0 to 1 and never back");
    bool checkReported = false;
    for (const double p : run.progress) {
        checkReported = checkReported || (p > 0.6 && p < 1.0);
    }
    // A file shorter than a decoder buffer is checked in one step.
    check(checkReported || spec.frames < static_cast<std::uint64_t>(spec.rate),
          label + ": the check is part of the progress");
}

void formats(const std::string &dir)
{
    for (const int rate : {44100, 48000}) {
        for (const int channels : {1, 2}) {
            roundTrip(dir, WavSpec{rate, channels, 16, static_cast<std::uint64_t>(rate) * 3});
        }
    }
    // Lengths that are not whole seconds, and one shorter than an encoder
    // block: the recorder's duration is in whole milliseconds.
    roundTrip(dir, WavSpec{44100, 2, 16, 44100 * 2 + 1237});
    roundTrip(dir, WavSpec{48000, 1, 16, 1000});
    roundTrip(dir, WavSpec{44100, 2, 16, 4608 * 3 + 1});
    roundTrip(dir, WavSpec{96000, 2, 16, 96000 / 3 + 7});
    roundTrip(dir, WavSpec{88200, 2, 16, 88200 * 2});
    roundTrip(dir, WavSpec{96000, 1, 16, 96000 * 2});
    // Headers: extensible, RF64, and a chunk before the format.
    roundTrip(dir, WavSpec{48000, 2, 16, 48000 * 2, true});
    roundTrip(dir, WavSpec{44100, 2, 16, 44100 * 2, false, true});
    WavSpec withList{44100, 1, 16, 44100};
    withList.extraChunk = true;
    roundTrip(dir, withList);
}

// 24 bit through Qt's 32 bit sample format: the low byte must survive.
void twentyFourBit(const std::string &dir)
{
    roundTrip(dir, WavSpec{48000, 2, 24, 48000 * 3});
    roundTrip(dir, WavSpec{96000, 1, 24, 96000 * 2, true});
    roundTrip(dir, WavSpec{44100, 2, 24, 44100 * 2});
}

void refusals(const std::string &dir)
{
    QtFlacRecordingEncoder encoder;
    const auto refused = [&](const std::string &label, const std::string &wav, const std::string &flac,
                             const std::string &reason) {
        QElapsedTimer timer;
        timer.start();
        const Run run = encode(encoder, wav, flac);
        check(!run.result.encoded && !run.result.cancelled, label + ": refused");
        check(run.result.error.find(reason) != std::string::npos,
              label + ": the reason says \"" + reason + "\": " + run.result.error);
        check(noTempBeside(flac), label + ": no temporary file");
        check(timer.elapsed() < 5000, label + ": refused at once, not after a stall");
        return run;
    };

    // Cut short: the data chunk says more than the file holds.
    {
        const std::string wav = dir + "/cut.wav";
        const std::string flac = dir + "/cut.flac";
        WavSpec spec{44100, 2, 16, 44100};
        spec.declaredDataBytes = 44100 * 4 * 2;
        recording_test::writeWav(wav, spec);
        refused("cut short", wav, flac, "cut short");
        check(!exists(flac), "cut short: no FLAC");
    }
    // Not a WAV.
    {
        const std::string wav = dir + "/text.wav";
        std::ofstream(pathFromUtf8(wav)) << "this is a text file, not audio at all, but long enough to be read\n";
        refused("not a WAV", wav, dir + "/text.flac", "not a WAV");
        check(!exists(dir + "/text.flac"), "not a WAV: no FLAC");
    }
    // A header that never got its length.
    {
        const std::string wav = dir + "/unfinished.wav";
        WavSpec spec{44100, 2, 16, 44100};
        spec.declaredDataBytes = 0xFFFFFFFFU;
        recording_test::writeWav(wav, spec);
        refused("unfinished", wav, dir + "/unfinished.flac", "not finished");
    }
    // A sample format that is not read: 8 bit, written by hand.
    {
        const std::string wav = dir + "/eight.wav";
        std::ofstream out(pathFromUtf8(wav), std::ios::binary);
        out.write("RIFF\x2c\x01\x00\x00WAVEfmt \x10\x00\x00\x00\x01\x00\x01\x00\x44\xac\x00\x00\x44\xac\x00\x00"
                  "\x01\x00\x08\x00" "data" "\x08\x01\x00\x00",
                  44);
        out.write(std::string(264, '\x80').data(), 264);
        out.close();
        refused("8 bit", wav, dir + "/eight.flac", "only 16 and 24 bit");
    }
    // A plain RIFF over 4 GB: its 32 bit sizes cannot say how long it is.
    // Grown with resize_file, which leaves a sparse file where the
    // filesystem can.
    {
        const std::string wav = dir + "/huge.wav";
        recording_test::writeWav(wav, WavSpec{44100, 2, 16, 44100});
        fs::resize_file(pathFromUtf8(wav), (std::uintmax_t{1} << 32) + 4096);
        refused("over 4 GB", wav, dir + "/huge.flac", "over 4 GB");
        fs::remove(pathFromUtf8(wav));
    }
    // A WAV that is not there.
    refused("missing", dir + "/nothing-here.wav", dir + "/nothing-here.flac", "could not be");

    const std::string good = dir + "/good.wav";
    recording_test::writeWav(good, WavSpec{44100, 2, 16, 44100});
    // A target that exists is not replaced.
    {
        const std::string flac = dir + "/existing.flac";
        std::ofstream(pathFromUtf8(flac)) << "keep me";
        refused("existing target", good, flac, "exists already");
        check(readAll(flac) == "keep me", "existing target: left as it was");
    }
    // A target folder that cannot be written: a file is where the folder
    // would be.
    {
        const std::string folder = dir + "/a-file-not-a-folder";
        std::ofstream(pathFromUtf8(folder)) << "x";
        refused("folder is a file", good, folder + "/out.flac", "not a folder");
    }
    // A folder where the temporary file would go.
    {
        const std::string flac = dir + "/blocked.flac";
        QDir().mkpath(QString::fromStdString(flac + RecordingEncodeTempSuffix + "/inside"));
        const Run run = encode(encoder, good, flac);
        check(!run.result.encoded && run.result.error.find("in the way") != std::string::npos,
              "temporary blocked: refused (" + run.result.error + ")");
        check(fs::is_directory(pathFromUtf8(flac + RecordingEncodeTempSuffix + "/inside")),
              "temporary blocked: the folder in the way is left alone");
        check(!exists(flac), "temporary blocked: no FLAC");
    }
}

// The check is what makes a FLAC count: a damaged one never gets the
// target's name.
void damaged(const std::string &dir)
{
    const WavSpec spec{44100, 2, 16, 44100 * 4};
    const std::string wav = dir + "/damage.wav";
    recording_test::writeWav(wav, spec);
    const auto run = [&](const std::string &label, const std::function<void(const std::string &)> &damage) {
        QtFlacRecordingEncoder encoder(5000);
        encoder.setBeforeCheckForTests(damage);
        const std::string flac = dir + "/damage-" + label + ".flac";
        const Run r = encode(encoder, wav, flac);
        check(!r.result.encoded && !r.result.cancelled && !r.result.error.empty(),
              label + ": the damaged FLAC is refused (" + r.result.error + ")");
        check(!exists(flac), label + ": nothing under the target's name");
        check(noTempBeside(flac), label + ": the damaged temporary file is taken away");
    };
    // The second half of the file gone.
    run("cut", [](const std::string &temp) {
        const auto size = fs::file_size(pathFromUtf8(temp));
        fs::resize_file(pathFromUtf8(temp), size / 2);
    });
    // Bytes in the middle of the audio changed, the size kept.
    run("changed", [](const std::string &temp) {
        std::fstream file(pathFromUtf8(temp), std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(static_cast<std::streamoff>(fs::file_size(pathFromUtf8(temp)) / 2));
        file.write("\x12\x34\x56\x78\x9a\xbc\xde\xf0", 8);
    });
}

void cancels(const std::string &dir)
{
    const WavSpec spec{48000, 2, 16, 48000 * 20};
    const std::string wav = dir + "/cancel.wav";
    recording_test::writeWav(wav, spec);
    // While encoding (the first 0.6 of the progress) and while checking.
    for (const double at : {0.2, 0.8}) {
        const std::string label = "cancel at " + std::to_string(at);
        const std::string flac = dir + "/cancel-" + std::to_string(static_cast<int>(at * 10)) + ".flac";
        QtFlacRecordingEncoder encoder;
        application::CancellationToken token;
        const Run run = encode(encoder, wav, flac, token, at);
        check(run.result.cancelled && !run.result.encoded && run.result.error.empty(),
              label + ": the answer is cancelled (" + run.result.error + ")");
        check(!run.progress.empty() && run.progress.back() < 1.0, label + ": stopped before the end");
        check(!exists(flac), label + ": no FLAC");
        check(noTempBeside(flac), label + ": no temporary file left");
    }
}

// The encoder in a worker thread with no event loop of its own running,
// the way the app calls it.
void workerThread(const std::string &dir)
{
    const WavSpec spec{44100, 2, 16, 44100 * 2};
    const std::string wav = dir + "/thread.wav";
    const std::string flac = dir + "/thread.flac";
    recording_test::writeWav(wav, spec);
    application::RecordingEncodeResult result;
    QThread *thread = QThread::create([&]() {
        QtFlacRecordingEncoder encoder;
        result = encoder.encode(wav, flac, {});
    });
    thread->start();
    // The main thread's loop runs meanwhile, as the GUI's would.
    QEventLoop loop;
    QObject::connect(thread, &QThread::finished, &loop, &QEventLoop::quit);
    if (!thread->isFinished()) {
        loop.exec();
    }
    thread->wait();
    delete thread;
    check(result.encoded, "worker thread: encoded (" + result.error + ")");
    check(recording_test::compareWithSignal(recording_test::decodeAll(flac), spec).empty(),
          "worker thread: the FLAC decodes to the signal");
}

// The process's peak memory so far, in bytes.
std::uint64_t peakMemory()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return counters.PeakWorkingSetSize;
    }
    return 0;
#else
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
#endif
}

void longRecording(const std::string &dir, int minutes)
{
    const WavSpec spec{44100, 2, 16, static_cast<std::uint64_t>(44100) * 60 * static_cast<std::uint64_t>(minutes)};
    const std::string wav = dir + "/long.wav";
    const std::string flac = dir + "/long.flac";
    if (!recording_test::writeWav(wav, spec)) {
        check(false, "long: the test could not write its WAV");
        return;
    }
    const std::uint64_t wavBytes = spec.frames * 4;
    const std::uint64_t before = peakMemory();
    QElapsedTimer timer;
    timer.start();
    double encodedAt = 0.0;
    qint64 encodingMs = 0;
    application::RecordingEncodeOptions options;
    options.progress = [&](double p) {
        if (encodedAt < 0.6 && p >= 0.6) {
            encodingMs = timer.elapsed();
        }
        encodedAt = p;
    };
    QtFlacRecordingEncoder encoder;
    const auto result = encoder.encode(wav, flac, options);
    const qint64 totalMs = timer.elapsed();
    const std::uint64_t after = peakMemory();
    check(result.encoded, "long: encoded (" + result.error + ")");
    check(result.framesVerified == spec.frames, "long: every frame checked");
    const std::uint64_t grew = after > before ? after - before : 0;
    std::cout << "long: " << minutes << " min stereo 16 bit WAV of " << wavBytes / 1000000 << " MB, FLAC of "
              << fs::file_size(pathFromUtf8(flac)) / 1000000 << " MB; encoding " << encodingMs << " ms, with the check "
              << totalMs << " ms; peak memory grew by " << grew / 1000000 << " MB\n";
    check(before > 0 && after > 0, "long: the peak memory can be read on this platform");
    check(grew < wavBytes / 4, "long: peak memory grew by " + std::to_string(grew / 1000000) + " MB, not by a part of the "
                                   + std::to_string(wavBytes / 1000000) + " MB WAV");
    fs::remove(pathFromUtf8(wav));
    fs::remove(pathFromUtf8(flac));
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::cerr << "usage: recording_encoder_test SCRATCH_DIR [--long [MINUTES]]\n";
        return 2;
    }
    if (!QtFlacRecordingEncoder::encodingAvailable()) {
        std::cerr << "FAILED: no FLAC encoding and decoding backend\n";
        return 1;
    }
    const QStringList args = QCoreApplication::arguments();
    const QString scratchRoot = args.at(1);
    QDir().mkpath(scratchRoot);
    // A name outside ASCII, as recordings and folders have.
    QTemporaryDir scratch(scratchRoot + QStringLiteral("/recording-encoder-éß-XXXXXX"));
    if (!scratch.isValid()) {
        std::cerr << "FAILED: no scratch directory\n";
        return 1;
    }
    const std::string dir = QDir(scratch.path()).absolutePath().toStdString();
    if (args.size() >= 3 && args.at(2) == QStringLiteral("--long")) {
        const int minutes = args.size() >= 4 ? args.at(3).toInt() : 10;
        longRecording(dir, minutes > 0 ? minutes : 10);
    } else {
        formats(dir);
        twentyFourBit(dir);
        refusals(dir);
        damaged(dir);
        cancels(dir);
        workerThread(dir);
    }
    if (failures > 0) {
        std::cerr << failures << " check(s) FAILED\n";
        return 1;
    }
    std::cout << "recording_encoder_test: all checks passed\n";
    return 0;
}
