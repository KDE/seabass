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
//     target;
//   - the FLAC's own MD5 (STREAMINFO) is the MD5 the test computes from the
//     signal it generated, a check that goes through no decoder;
//   - a valid FLAC of other audio with its MD5 zeroed, and a 24 bit FLAC
//     that differs from the WAV in the lowest byte of one sample only, put
//     in the place of the encoded file: the comparison itself refuses them;
//   - a WAV with bytes after its data chunk: chunks, zero bytes to the end
//     (up to a second of audio), an ID3v1 block at the end, and after an
//     odd data chunk a chunk without the pad byte that ends exactly at the
//     end are fine; anything else (a stale size with frames after it, a raw
//     ID3v2 tag, a stray pad byte before a LIST, more than a second of
//     zeros) is refused with the two lengths, counted in whole frames, the
//     choice to keep an unfinished one or not;
//   - a recording whose header was not completed (a data size of 0 or
//     0xFFFFFFFF in a plain RIFF, one past the end), 16 and 24 bit, with a
//     partial last frame: refused, saying so with both lengths where there
//     are two, without the choice; with it, every whole frame is kept, the
//     partial frame reported, STREAMINFO's total the frames kept and the
//     FLAC the signal of those frames;
//   - RF64 with a data size of 0 is refused with a plain reason, and a file
//     without one whole frame holds no audio;
//   - the temporary file is named "<FLAC>.<16 hex>.seabass-encode-tmp",
//     and the recorder writes under that name (no extension added);
//   - a temporary name too long for the filesystem: refused, saying so;
//   - a rename through a hard link whose old name could not be removed:
//     the second name is removed again, and when that is not possible, its
//     path is said;
//   - the temporary file taken away or changed after the check: refused,
//     no empty or other file under the target's name; a file that appears
//     at the target right before the rename: not replaced;
//   - another job's temporary file beside the target is left alone; the
//     FLAC is not executable.
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
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#include "infrastructure/audio/flac_stream_info.hpp"
#include "infrastructure/audio/qt_flac_recording_encoder.hpp"
#include "infrastructure/audio/wav_pcm_file.hpp"
#include "infrastructure/no_replace_rename.hpp"
#include "recording_test_support.hpp"

#if !defined(_WIN32)
#include <cerrno>
#include <unistd.h>
#endif

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

// The encoder's temporary files beside `flac`: names that start with the
// FLAC's name and end in the suffix.
std::vector<std::string> tempsBeside(const std::string &flac)
{
    const fs::path path = pathFromUtf8(flac);
    const std::string prefix = pathToUtf8(path.filename()) + ".";
    const std::string suffix = RecordingEncodeTempSuffix;
    std::vector<std::string> found;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(path.parent_path(), ec)) {
        const std::string name = pathToUtf8(entry.path().filename());
        if (name.size() > prefix.size() + suffix.size() && name.compare(0, prefix.size(), prefix) == 0
            && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            found.push_back(name);
        }
    }
    return found;
}

// Nothing of the encoder's is left beside `flac`.
bool noTempBeside(const std::string &flac)
{
    return tempsBeside(flac).empty();
}

// The FLAC's STREAMINFO MD5 set to zero, as an encoder that did not
// compute it leaves it. STREAMINFO is the first block, right after
// "fLaC" and its four byte block header; the MD5 is its last 16 bytes.
bool zeroMd5(const std::string &flac)
{
    const auto info = infrastructure::audio::readFlacStreamInfo(pathFromUtf8(flac));
    std::fstream file(pathFromUtf8(flac), std::ios::in | std::ios::out | std::ios::binary);
    char head[42] = {};
    file.read(head, 42);
    if (!info || !file || std::string(head, 4) != "fLaC"
        || !std::equal(info->md5.begin(), info->md5.end(), reinterpret_cast<unsigned char *>(head) + 26)) {
        return false;
    }
    file.seekp(26);
    file.write(std::string(16, '\0').data(), 16);
    return static_cast<bool>(file);
}

void copyOver(const std::string &from, const std::string &to)
{
    fs::copy_file(pathFromUtf8(from), pathFromUtf8(to), fs::copy_options::overwrite_existing);
}

struct Run
{
    application::RecordingEncodeResult result;
    std::vector<double> progress;
};

Run encode(QtFlacRecordingEncoder &encoder, const std::string &wav, const std::string &flac,
           application::CancellationToken cancel = application::CancellationToken::none(),
           double cancelAt = 2.0, bool keepUnfinished = false)
{
    Run run;
    application::RecordingEncodeOptions options;
    options.cancel = cancel;
    options.keepUnfinished = keepUnfinished;
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
    // Without any decoder: the FLAC's own MD5 against the signal's.
    const auto info = infrastructure::audio::readFlacStreamInfo(pathFromUtf8(flac));
    check(info && info->md5 == recording_test::signalMd5(spec),
          label + ": the FLAC's STREAMINFO MD5 is the MD5 of the generated samples");
#if !defined(_WIN32)
    const fs::perms perms = fs::status(pathFromUtf8(flac)).permissions();
    check((perms & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) == fs::perms::none,
          label + ": the FLAC is not executable");
#endif

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
    // Chunks after the audio, as recorders write them: a LIST, and an odd
    // sized one at the end without its pad byte.
    WavSpec listAfter{44100, 2, 16, 44100};
    listAfter.trailing = std::string("LIST\x0a\x00\x00\x00INFOabcdef", 18);
    roundTrip(dir, listAfter);
    WavSpec oddAfter{48000, 2, 16, 48000};
    oddAfter.trailing = std::string("cue \x04\x00\x00\x00" "abcdjunk\x03\x00\x00\x00xyz", 23);
    roundTrip(dir, oddAfter);
    // Zero bytes to the end (padding), after the audio and after a chunk.
    WavSpec padded{44100, 2, 16, 44100 + 3};
    padded.trailing = std::string(4093, '\0');
    roundTrip(dir, padded);
    WavSpec chunkThenPadding{44100, 1, 16, 44100 + 5};
    chunkThenPadding.trailing = std::string("LIST\x04\x00\x00\x00INFO", 12) + std::string(777, '\0');
    roundTrip(dir, chunkThenPadding);
    // An ID3v1 block at the very end.
    WavSpec id3{44100, 2, 16, 44100 + 9};
    id3.trailing = "TAG" + std::string("Late Set") + std::string(117, ' ');
    roundTrip(dir, id3);
    // An odd data chunk (24 bit mono, an odd number of frames) and a chunk
    // right after it without the pad byte, ending at the end of the file.
    WavSpec oddData{44100, 1, 24, 44101};
    oddData.trailing = std::string("LIST\x04\x00\x00\x00INFO", 12);
    roundTrip(dir, oddData);
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
        refused("cut short", wav, flac, "it declares 0:02 of audio, and the file holds 0:01");
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
        refused("unfinished", wav, dir + "/unfinished.flac", "its header was not completed: it does not say how long");
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
    // Another job's temporary file (or a leftover of one that was
    // killed) beside the target: left alone, and the encoding works.
    {
        const std::string flac = dir + "/beside.flac";
        const std::string other = flac + ".0123456789abcdef" + RecordingEncodeTempSuffix;
        std::ofstream(pathFromUtf8(other)) << "another job's file";
        const Run run = encode(encoder, good, flac);
        check(run.result.encoded, "another temporary file beside: encoded (" + run.result.error + ")");
        check(readAll(other) == "another job's file", "another temporary file beside: left as it was");
        check(tempsBeside(flac) == std::vector<std::string>{pathToUtf8(pathFromUtf8(other).filename())},
              "another temporary file beside: only that one is there afterwards");
    }
    // The FLAC under the recording's own name: refused, the recording
    // untouched.
    {
        const std::string wav = dir + "/own-name.wav";
        recording_test::writeWav(wav, WavSpec{44100, 2, 16, 44100});
        const std::string before = readAll(wav);
        refused("the recording's own name", wav, wav, "the recording itself");
        check(readAll(wav) == before, "the recording's own name: the recording is as it was");
    }
}

// A header that declares less than the file holds, and garbage after the
// audio: the header does not match the file, which is refused with the
// two lengths and nothing said of a cause. Never offered as unfinished.
void trailingBytes(const std::string &dir)
{
    QtFlacRecordingEncoder encoder;
    const auto refusedWith = [&](const std::string &label, const WavSpec &spec, const std::string &reason) {
        const std::string wav = dir + "/" + label + ".wav";
        const std::string flac = dir + "/" + label + ".flac";
        recording_test::writeWav(wav, spec);
        const Run run = encode(encoder, wav, flac);
        check(!run.result.encoded && !run.result.cancelled && !run.result.unfinished, label + ": refused, not as unfinished");
        check(run.result.error.find(reason) != std::string::npos,
              label + ": the reason says \"" + reason + "\": " + run.result.error);
        check(run.result.error.find("the header does not match the file") != std::string::npos
                  && run.result.error.find("pulled") == std::string::npos
                  && run.result.error.find("power") == std::string::npos,
              label + ": only what is known is said: " + run.result.error);
        check(!exists(flac) && noTempBeside(flac), label + ": nothing written");
        // Not with the choice either: it is not offered.
        const Run chosen = encode(encoder, wav, flac, application::CancellationToken::none(), 2.0, true);
        check(!chosen.result.encoded && !exists(flac) && noTempBeside(flac), label + ": with the choice, refused too");
    };
    // Two minutes of frames, the header says one (a stale size).
    WavSpec stale{44100, 2, 16, 44100 * 120};
    stale.declaredDataBytes = 44100 * 4 * 60;
    refusedWith("stale-size", stale, "its header declares 1:00 of audio and the file could hold 2:00");
    // A finished file and a raw ID3v2 tag after it.
    WavSpec id3v2{44100, 2, 16, 44100 * 3};
    id3v2.trailing = std::string("ID3\x04\x00\x00\x00\x00\x02\x01", 10) + "TIT2" + std::string(253, '\x05');
    refusedWith("raw-id3v2", id3v2, "its header declares 0:03 of audio and the file could hold 0:03 (267 bytes more");
    // A stray pad byte before a LIST chunk (the data chunk is even).
    WavSpec strayPad{44100, 2, 16, 44100 * 3};
    strayPad.trailing = std::string(1, '\0') + std::string("LIST\x04\x00\x00\x00INFO", 12);
    refusedWith("stray-pad-before-list", strayPad,
                "its header declares 0:03 of audio and the file could hold 0:03 (13 bytes more");
    // More than a second of zero bytes after the audio: digital silence
    // that long is not taken for padding.
    WavSpec longZeros{44100, 2, 16, 44100 * 2};
    longZeros.trailing = std::string(44100 * 4 + 4, '\0');
    refusedWith("zeros-over-a-second", longZeros, "its header declares 0:02 of audio and the file could hold 0:03");
    WavSpec chunkThenLongZeros{44100, 2, 16, 44100 * 2};
    chunkThenLongZeros.trailing = std::string("LIST\x04\x00\x00\x00INFO", 12) + std::string(44100 * 4 + 1, '\0');
    refusedWith("chunk-then-zeros-over-a-second", chunkThenLongZeros, "the header does not match the file");
    // Not zero, not a chunk, not whole frames: refused, the room counted
    // in whole frames only.
    WavSpec garbage{44100, 2, 16, 44100};
    garbage.trailing = std::string(4095, '\x7f');
    refusedWith("garbage-after", garbage, "the header does not match the file");
    WavSpec roomInFrames{44100, 2, 16, 44100 * 2};
    roomInFrames.declaredDataBytes = 44100 * 4;
    roomInFrames.trailing = std::string(44100 * 4 * 59 + 3, '\x01');  // 59 s and 3 bytes of garbage
    refusedWith("room-in-frames", roomInFrames, "its header declares 0:01 of audio and the file could hold 1:01");
    // Zero bytes that stop short of the end are not padding.
    WavSpec zerosThenJunk{44100, 2, 16, 44100};
    zerosThenJunk.trailing = std::string(100, '\0') + "junk!";
    refusedWith("zeros-then-junk", zerosThenJunk, "the header does not match the file");
    // An ID3v1 block that is not at the very end.
    WavSpec id3NotAtEnd{44100, 2, 16, 44100};
    id3NotAtEnd.trailing = "TAG" + std::string(125, ' ') + "x";
    refusedWith("id3-not-at-end", id3NotAtEnd, "the header does not match the file");
    // After an odd data chunk, a chunk without the pad byte that does not
    // end at the end of the file.
    WavSpec oddDataRunsOn{44100, 1, 24, 44101};
    oddDataRunsOn.trailing = std::string("LIST\x04\x00\x00\x00INFO", 12) + "zz";
    refusedWith("odd-data-runs-on", oddDataRunsOn, "the header does not match the file");
    // ... nor with zero bytes after it: the walk without the pad byte
    // takes chunks only, to the very end.
    WavSpec oddDataThenZeros{44100, 1, 24, 44101};
    oddDataThenZeros.trailing = std::string("LIST\x04\x00\x00\x00INFO", 12) + std::string(4, '\0');
    refusedWith("odd-data-then-zeros", oddDataThenZeros, "the header does not match the file");
    WavSpec runsPast{44100, 2, 16, 44100};
    runsPast.trailing = std::string("LIST\xff\x00\x00\x00INFO", 12);
    refusedWith("chunk-runs-past", runsPast, "the header does not match the file");
    WavSpec secondData{44100, 2, 16, 44100};
    secondData.trailing = std::string("data\x04\x00\x00\x00" "abcd", 12) + "x";
    refusedWith("second-data", secondData, "the header does not match the file");
}

// A recording the player did not finish, in each header state it leaves.
void unfinishedHeaders(const std::string &dir)
{
    struct Case
    {
        std::string label;
        WavSpec spec;
        std::uint64_t partialBytes;
    };
    std::vector<Case> cases;
    for (const int bits : {16, 24}) {
        const std::uint64_t frames = 48000 * 2 + 77;
        const std::uint32_t align = static_cast<std::uint32_t>(bits / 8 * 2);
        const std::string b = std::to_string(bits);
        // No size: 0 and 0xFFFFFFFF, a partial last frame after the audio.
        WavSpec noSize{48000, 2, bits, frames};
        noSize.declaredDataBytes = 0xFFFFFFFFU;
        noSize.trailing = std::string(align - 1, '\x11');
        cases.push_back({"unfinished-ffffffff-" + b, noSize, align - 1});
        WavSpec zero{48000, 2, bits, frames};
        zero.declaredDataBytes = 0;
        cases.push_back({"unfinished-zero-" + b, zero, 0});
        // More than the file holds, a partial frame at the end.
        WavSpec past{48000, 2, bits, frames};
        past.declaredDataBytes = static_cast<std::uint32_t>((frames + 48000) * align);
        past.trailing = std::string(1, '\x22');
        cases.push_back({"unfinished-past-end-" + b, past, 1});
        // Cut short ending in a partial frame of all but one byte.
        WavSpec pastPartial{48000, 2, bits, frames};
        pastPartial.declaredDataBytes = static_cast<std::uint32_t>((frames + 48000 * 60) * align);
        pastPartial.trailing = std::string(align - 1, '\x33');
        cases.push_back({"unfinished-past-end-partial-" + b, pastPartial, align - 1});
    }
    for (const Case &c : cases) {
        const std::string wav = dir + "/" + c.label + ".wav";
        if (!recording_test::writeWav(wav, c.spec)) {
            check(false, c.label + ": the test could not write its WAV");
            continue;
        }
        const std::string before = readAll(wav);
        QtFlacRecordingEncoder encoder;
        {
            const std::string flac = dir + "/" + c.label + "-refused.flac";
            const Run run = encode(encoder, wav, flac);
            check(!run.result.encoded && run.result.unfinished
                      && run.result.error.find("its header was not completed") != std::string::npos,
                  c.label + ": without the choice, refused as unfinished (" + run.result.error + ")");
            check(run.result.framesInWav == c.spec.frames, c.label + ": the frames the file holds are reported");
            check(run.result.sampleRate == c.spec.rate, c.label + ": and the rate, so a length can be said");
            const std::uint32_t align = static_cast<std::uint32_t>(c.spec.bits / 8 * c.spec.channels);
            const std::uint32_t declared = *c.spec.declaredDataBytes;
            if (declared == 0 || declared == 0xFFFFFFFFU) {
                check(!run.result.framesDeclared, c.label + ": no declared length");
            } else {
                check(run.result.framesDeclared == std::optional<std::uint64_t>(declared / align),
                      c.label + ": the declared length is reported");
                const std::string both = "it declares "
                    + infrastructure::audio::wavDuration(declared / align, c.spec.rate) + " of audio, and the file holds "
                    + infrastructure::audio::wavDuration(c.spec.frames, c.spec.rate);
                check(run.result.error.find(both) != std::string::npos,
                      c.label + ": both lengths said (\"" + both + "\"): " + run.result.error);
            }
            check(!exists(flac) && noTempBeside(flac), c.label + ": without the choice, nothing written");
        }
        const std::string flac = dir + "/" + c.label + ".flac";
        const Run run = encode(encoder, wav, flac, application::CancellationToken::none(), 2.0, true);
        check(run.result.encoded && run.result.unfinished, c.label + ": with the choice, encoded (" + run.result.error + ")");
        check(run.result.framesInWav == c.spec.frames && run.result.framesVerified == c.spec.frames,
              c.label + ": every whole frame kept and checked (" + std::to_string(run.result.framesVerified) + ")");
        check(run.result.partialFrameBytesDropped == c.partialBytes,
              c.label + ": the partial last frame is reported (" + std::to_string(run.result.partialFrameBytesDropped)
                  + " bytes)");
        const auto info = infrastructure::audio::readFlacStreamInfo(pathFromUtf8(flac));
        check(info && info->totalFrames == c.spec.frames, c.label + ": STREAMINFO's total is the frames kept");
        check(info && info->md5 == recording_test::signalMd5(c.spec), c.label + ": and its MD5 is of those frames");
        const std::string mismatch = recording_test::compareWithSignal(recording_test::decodeAll(flac), c.spec);
        check(mismatch.empty(), c.label + ": the FLAC decodes to the frames the file held: " + mismatch);
        check(readAll(wav) == before, c.label + ": the WAV is not written to");
    }
    // A finished WAV with the choice set: read as declared, not to the end.
    {
        WavSpec spec{44100, 2, 16, 44100};
        spec.trailing = std::string("LIST\x04\x00\x00\x00INFO", 12);
        const std::string wav = dir + "/finished-with-choice.wav";
        recording_test::writeWav(wav, spec);
        QtFlacRecordingEncoder encoder;
        const Run run = encode(encoder, wav, dir + "/finished-with-choice.flac", application::CancellationToken::none(),
                               2.0, true);
        check(run.result.encoded && !run.result.unfinished && run.result.framesInWav == spec.frames,
              "a finished WAV with the choice: read as its header declares");
    }
}

// What the reader makes of headers, without encoding: RF64 sizes, how much
// zero padding a finished file may have, and a file without one frame.
void headerReadings(const std::string &dir)
{
    using infrastructure::audio::WavPcmFile;
    using infrastructure::audio::WavReading;
    const auto read = [&](const std::string &label, const WavSpec &spec, WavReading reading) {
        const std::string wav = dir + "/" + label + ".wav";
        recording_test::writeWav(wav, spec);
        return std::make_unique<WavPcmFile>(pathFromUtf8(wav), reading);
    };
    // RF64 with a data size of 0, in ds64 or in the data chunk too: not a
    // recorder that stopped, refused with a plain reason, either way.
    for (const auto chunkSize : {std::uint32_t{0xFFFFFFFFU}, std::uint32_t{0}}) {
        WavSpec spec{48000, 2, 24, 48000};
        spec.rf64 = true;
        spec.ds64DataBytes = 0;
        spec.declaredDataBytes = chunkSize;
        for (const WavReading reading : {WavReading::AsDeclared, WavReading::ToEndOfFile}) {
            const auto wav = read("rf64-zero", spec, reading);
            check(wav->state() == WavPcmFile::State::Refused && !wav->unfinished()
                      && wav->error().find("RF64 header does not say how long its audio is (the audio size in its "
                                           "ds64 chunk is 0)") != std::string::npos,
                  "RF64, ds64 data size 0, data chunk " + std::to_string(chunkSize) + ": refused, not unfinished: "
                      + wav->error());
        }
    }
    // RF64 whose data chunk says 0 and whose ds64 has the size: read as ds64 says.
    {
        WavSpec spec{48000, 2, 24, 48000};
        spec.rf64 = true;
        spec.declaredDataBytes = 0;
        const auto wav = read("rf64-chunk-zero", spec, WavReading::AsDeclared);
        check(wav->isOpen() && wav->layout().frames == spec.frames, "RF64, data chunk 0, ds64 right: read as ds64 says");
    }
    // Zero padding of exactly a second: a finished file.
    {
        WavSpec spec{44100, 2, 16, 44100};
        spec.trailing = std::string(44100 * 4, '\0');
        const auto wav = read("zeros-one-second", spec, WavReading::AsDeclared);
        check(wav->isOpen() && !wav->unfinished() && wav->layout().frames == spec.frames,
              "a second of zero padding: finished, read as declared (" + wav->error() + ")");
    }
    // ... one byte more: refused, both lengths.
    {
        WavSpec spec{44100, 2, 16, 44100};
        spec.trailing = std::string(44100 * 4 + 1, '\0');
        const auto wav = read("zeros-one-second-and-a-byte", spec, WavReading::ToEndOfFile);
        check(wav->state() == WavPcmFile::State::Refused && !wav->unfinished()
                  && wav->error() == "its header declares 0:01 of audio and the file could hold 0:02: the header does "
                                     "not match the file",
              "a second and a byte of zeros: refused with both lengths: " + wav->error());
    }
    // No whole frame held: refused, holds no audio, not offered as unfinished.
    for (const std::uint32_t declared : {std::uint32_t{0}, std::uint32_t{0xFFFFFFFFU}, std::uint32_t{4000}}) {
        WavSpec spec{44100, 2, 16, 0};
        spec.declaredDataBytes = declared;
        spec.trailing = std::string(3, '\x01');
        for (const WavReading reading : {WavReading::AsDeclared, WavReading::ToEndOfFile}) {
            const auto wav = read("no-frame", spec, reading);
            check(wav->state() == WavPcmFile::State::Refused && wav->holdsNoAudio() && !wav->unfinished()
                      && wav->error() == "it holds no audio",
                  "no whole frame, declared " + std::to_string(declared) + ": holds no audio: " + wav->error());
        }
    }
    {
        const auto wav = read("one-frame", WavSpec{44100, 2, 16, 1}, WavReading::AsDeclared);
        check(wav->isOpen() && !wav->holdsNoAudio(), "one frame: open, holds audio");
    }
}

// The temporary file's name, and what the filesystem cannot take.
void temporaryNames(const std::string &dir)
{
    const WavSpec spec{44100, 2, 16, 44100};
    const std::string wav = dir + "/names.wav";
    recording_test::writeWav(wav, spec);
    {
        QtFlacRecordingEncoder encoder;
        std::string seen;
        bool there = false;
        encoder.setBeforeCheckForTests([&](const std::string &temp) {
            seen = temp;
            there = exists(temp);
        });
        const std::string flac = dir + "/named.flac";
        const Run run = encode(encoder, wav, flac);
        const std::string name = pathToUtf8(pathFromUtf8(seen).filename());
        const std::string suffix = ".seabass-encode-tmp";  // as written, not the constant
        check(run.result.encoded, "temporary name: encoded (" + run.result.error + ")");
        check(name.size() == std::string("named.flac").size() + 1 + 16 + suffix.size()
                  && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0
                  && name.find(".flac.") == std::string("named").size(),
              "temporary name: <FLAC>.<16 hex>.seabass-encode-tmp, no audio extension last: " + name);
        check(there, "temporary name: the recorder wrote under exactly that name");
    }
    // A name the FLAC fits and its temporary name does not.
    {
        QtFlacRecordingEncoder encoder;
        const std::string flac = dir + "/" + std::string(230, 'n') + ".flac";
        const Run run = encode(encoder, wav, flac);
        check(!run.result.encoded && run.result.error.find("too long") != std::string::npos
                  && run.result.error.find("recording itself") == std::string::npos,
              "a temporary name too long: refused, saying so (" + run.result.error + ")");
        check(!exists(flac), "a temporary name too long: nothing written");
    }
#if !defined(_WIN32)
    using infrastructure::NoReplaceRenameCalls;
    using infrastructure::setNoReplaceRenameCallsForTests;
    const auto noAtomic = [](const char *, const char *) {
        errno = EINVAL;
        return -1;
    };
    // The old name could not be removed by the rename: removed again.
    {
        setNoReplaceRenameCallsForTests({noAtomic, {}, [](const char *) {
                                             errno = EBUSY;
                                             return -1;
                                         }});
        QtFlacRecordingEncoder encoder;
        const std::string flac = dir + "/second-name.flac";
        const Run run = encode(encoder, wav, flac);
        setNoReplaceRenameCallsForTests({});
        check(run.result.encoded && run.result.notes.empty(),
              "old name left by the rename: encoded, nothing to say (" + run.result.error + ")");
        check(noTempBeside(flac), "old name left by the rename: removed again");
    }
    // ... and when that is not possible either, its path is said. Here the
    // old name is another file by then, which is never removed.
    {
        setNoReplaceRenameCallsForTests({noAtomic, {}, [](const char *path) {
                                             ::unlink(path);
                                             std::ofstream(path) << "another file now";
                                             errno = EBUSY;
                                             return -1;
                                         }});
        QtFlacRecordingEncoder encoder;
        const std::string flac = dir + "/second-name-stays.flac";
        const Run run = encode(encoder, wav, flac);
        setNoReplaceRenameCallsForTests({});
        const std::vector<std::string> left = tempsBeside(flac);
        check(run.result.encoded && left.size() == 1 && run.result.notes.size() == 1
                  && run.result.notes.front().find(left.empty() ? std::string("?") : left.front()) != std::string::npos,
              "a second name that stays: encoded, and its path said: "
                  + (run.result.notes.empty() ? std::string() : run.result.notes.front()));
    }
#endif
}

// The comparison itself, with the MD5 out of the way: a FLAC that is valid
// in every other respect but holds other samples is refused.
void comparisonAlone(const std::string &dir)
{
    const auto refusedWith = [&](const std::string &label, const WavSpec &spec, const WavSpec &other,
                                 const std::function<void(const std::string &)> &changeOtherWav) {
        const std::string wav = dir + "/" + label + ".wav";
        const std::string otherWav = dir + "/" + label + "-other.wav";
        const std::string otherFlac = dir + "/" + label + "-other.flac";
        recording_test::writeWav(wav, spec);
        recording_test::writeWav(otherWav, other);
        if (changeOtherWav) {
            changeOtherWav(otherWav);
        }
        QtFlacRecordingEncoder plain;
        const Run made = encode(plain, otherWav, otherFlac);
        check(made.result.encoded, label + ": the other FLAC is made (" + made.result.error + ")");
        check(zeroMd5(otherFlac), label + ": the other FLAC's MD5 is zeroed");
        QtFlacRecordingEncoder encoder(5000);
        encoder.setBeforeCheckForTests([&](const std::string &temp) { copyOver(otherFlac, temp); });
        const std::string flac = dir + "/" + label + ".flac";
        const Run run = encode(encoder, wav, flac);
        check(!run.result.encoded && run.result.error.find("differs from the recording") != std::string::npos,
              label + ": refused by the comparison (" + run.result.error + ")");
        check(!exists(flac) && noTempBeside(flac), label + ": nothing under the target's name, no temporary file");
    };
    // Other audio of the same format and length.
    WavSpec other{44100, 2, 16, 44100 * 2};
    other.variant = 7;
    refusedWith("other-audio", WavSpec{44100, 2, 16, 44100 * 2}, other, {});
    // 24 bit, one sample's lowest byte one higher, the rest the same.
    const WavSpec deep{48000, 2, 24, 48000 * 2};
    refusedWith("lowest-byte", deep, deep, [](const std::string &path) {
        std::fstream file(pathFromUtf8(path), std::ios::in | std::ios::out | std::ios::binary);
        // Frame 30000, channel 2: its lowest byte, after the 44 byte header.
        const std::streamoff at = 44 + 30000 * 6 + 3;
        char byte = 0;
        file.seekg(at);
        file.read(&byte, 1);
        byte = static_cast<char>(byte ^ 0x01);
        file.seekp(at);
        file.write(&byte, 1);
    });
}

// The file that gets the target's name is the file that was checked.
void placement(const std::string &dir)
{
    const WavSpec spec{44100, 2, 16, 44100 * 2};
    const std::string wav = dir + "/place.wav";
    recording_test::writeWav(wav, spec);
    // The temporary file taken away after the check (another program, a
    // sweep): no empty file is made and named.
    {
        QtFlacRecordingEncoder encoder;
        encoder.setBeforePlaceForTests([](const std::string &temp) { fs::remove(pathFromUtf8(temp)); });
        const std::string flac = dir + "/place-vanished.flac";
        const Run run = encode(encoder, wav, flac);
        check(!run.result.encoded && run.result.error.find("could not be flushed") != std::string::npos,
              "temporary file vanished: refused at the flush (" + run.result.error + ")");
        check(!exists(flac), "temporary file vanished: nothing under the target's name");
        check(noTempBeside(flac), "temporary file vanished: no temporary file made either");
    }
    // The temporary file changed after the check: refused.
    {
        QtFlacRecordingEncoder encoder;
        encoder.setBeforePlaceForTests([](const std::string &temp) {
            fs::resize_file(pathFromUtf8(temp), fs::file_size(pathFromUtf8(temp)) / 2);
        });
        const std::string flac = dir + "/place-changed.flac";
        const Run run = encode(encoder, wav, flac);
        check(!run.result.encoded && run.result.error.find("changed after it was checked") != std::string::npos,
              "temporary file changed: refused (" + run.result.error + ")");
        check(!exists(flac) && noTempBeside(flac), "temporary file changed: nothing named, nothing left");
    }
    // A file appears at the target after the last look, right before the
    // rename: the rename does not replace it.
    {
        QtFlacRecordingEncoder encoder;
        const std::string flac = dir + "/place-appeared.flac";
        encoder.setBeforeRenameForTests([&flac](const std::string &) {
            std::ofstream(pathFromUtf8(flac)) << "someone else's file";
        });
        const Run run = encode(encoder, wav, flac);
        check(!run.result.encoded && run.result.error.find("is not replaced") != std::string::npos,
              "target appeared before the rename: refused (" + run.result.error + ")");
        check(readAll(flac) == "someone else's file", "target appeared before the rename: it is not replaced");
        check(noTempBeside(flac), "target appeared before the rename: the temporary file is taken away");
    }
    // A progress callback that throws at the end, after the FLAC has its
    // name: the FLAC still counts.
    {
        QtFlacRecordingEncoder encoder;
        const std::string flac = dir + "/place-throwing-progress.flac";
        application::RecordingEncodeOptions options;
        options.progress = [](double fraction) {
            if (fraction >= 1.0) {
                throw std::runtime_error("the progress bar broke");
            }
        };
        const auto result = encoder.encode(wav, flac, options);
        check(result.encoded && result.error.empty(),
              "a throwing progress at the end: still encoded (" + result.error + ")");
        check(recording_test::compareWithSignal(recording_test::decodeAll(flac), spec).empty(),
              "a throwing progress at the end: the FLAC is there and whole");
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
        trailingBytes(dir);
        unfinishedHeaders(dir);
        headerReadings(dir);
        temporaryNames(dir);
        comparisonAlone(dir);
        placement(dir);
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
