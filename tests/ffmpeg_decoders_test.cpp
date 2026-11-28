// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The silence probe, the alignment probe and the detail waveform through
// the decoder on FFmpeg's libraries against the same three through Qt
// Multimedia: the same file must give the same answer through both, to
// the last digit and the last sample, or a duplicate lines up one way
// and not the other. Every audio file of the fixture folders is put
// through each, once with each decoder.
//
// usage: ffmpeg_decoders_test DIR...

#include <QCoreApplication>
#include <QDir>

#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "infrastructure/audio/analysis_pcm_source.hpp"
#include "infrastructure/audio/qt_multimedia_alignment_probe.hpp"
#include "infrastructure/audio/qt_multimedia_detail_waveform.hpp"
#include "infrastructure/audio/qt_multimedia_pcm_source.hpp"
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

struct Mono
{
    bool decoded = false;
    int sampleRate = 0;
    std::vector<float> samples;
    bool operator==(const Mono &other) const = default;
};

Mono detail(const std::string &path)
{
    Mono mono;
    mono.decoded = decodeMono(path, [&](const float *samples, std::size_t count, int sampleRate) {
        mono.sampleRate = sampleRate;
        mono.samples.insert(mono.samples.end(), samples, samples + count);
    });
    return mono;
}

bool sameKept(const std::shared_ptr<const seabass::domain::alignment::KeptSamples> &a,
              const std::shared_ptr<const seabass::domain::alignment::KeptSamples> &b)
{
    if (!a || !b) {
        return !a && !b;
    }
    if (a->total != b->total || a->pieces.size() != b->pieces.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a->pieces.size(); ++i) {
        if (a->pieces[i].start != b->pieces[i].start || a->pieces[i].samples != b->pieces[i].samples) {
            return false;
        }
    }
    return true;
}

void compare(const std::string &path, const std::string &label)
{
    // Through Qt Multimedia.
    setOwnDecoderInUseForTesting(false);
    check(!ownDecoderInUse(), "the test can ask for Qt's decoder");
    const std::optional<seabass::domain::AudioContentSpan> qtSpan = QtMultimediaSilenceProbe(-60.0, 120000).measure(path);
    const auto qtKept = QtMultimediaAlignmentProbe().decoded(path);
    const Mono qtMono = detail(path);
    // And through FFmpeg's libraries.
    setOwnDecoderInUseForTesting(true);
    check(ownDecoderInUse(), "this build has the decoder on FFmpeg's libraries");
    const std::optional<seabass::domain::AudioContentSpan> span = QtMultimediaSilenceProbe(-60.0, 120000).measure(path);
    const auto kept = QtMultimediaAlignmentProbe().decoded(path);
    const Mono mono = detail(path);

    check(qtSpan.has_value() && qtKept && qtMono.decoded, label + ": Qt's decoder reads it for all three");
    check(span.has_value() == qtSpan.has_value()
              && (!span
                  || (span->totalSeconds == qtSpan->totalSeconds && span->leadingSilenceSeconds == qtSpan->leadingSilenceSeconds
                      && span->trailingSilenceSeconds == qtSpan->trailingSilenceSeconds)),
          label + ": the silence probe measures the same");
    check(sameKept(kept, qtKept), label + ": the alignment probe keeps the same samples");
    check(mono == qtMono, label + ": the detail waveform is fed the same samples (" + std::to_string(mono.samples.size())
                              + " against " + std::to_string(qtMono.samples.size()) + ")");
    std::cout << label << ": " << (span ? std::to_string(span->totalSeconds) : std::string("no span")) << " s, kept "
              << (kept ? kept->total : 0) << ", detail " << mono.samples.size() << " samples\n";
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::cerr << "usage: ffmpeg_decoders_test DIR...\n";
        return 2;
    }
    // Qt's own, asked of a class that has no other decoder: the probes
    // say "available" for the decoder on FFmpeg's libraries too.
    if (!QtMultimediaPcmSource::decodingAvailable()) {
        std::cerr << "FAILED: no audio decoding backend to compare with\n";
        return 1;
    }
    int compared = 0;
    for (int i = 1; i < argc; ++i) {
        const QDir dir(QCoreApplication::arguments().at(i));
        const QStringList files =
            dir.entryList({"*.mp3", "*.flac", "*.wav", "*.m4a", "*.ogg", "*.aiff"}, QDir::Files, QDir::Name);
        check(!files.isEmpty(), std::string("audio files in ") + argv[i]);
        for (const QString &name : files) {
            compare(dir.filePath(name).toStdString(), name.toStdString());
            ++compared;
        }
    }
    // A file that is not there and one that is not audio: no answer from
    // any of the three, through the decoder on FFmpeg's libraries.
    setOwnDecoderInUseForTesting(true);
    for (const std::string &name : {std::string("/no-such-file.mp3"), std::string("/README.md")}) {
        const std::string path = std::string(argv[1]) + name;
        check(!QtMultimediaSilenceProbe().measure(path).has_value(), name + ": no silence span");
        check(!QtMultimediaAlignmentProbe().decoded(path), name + ": nothing kept for alignment");
        check(!detail(path).decoded, name + ": no detail waveform");
    }
    // What ends a decode early ends it through both decoders: a timeout
    // that has run out before the first sample, and a cancel after some.
    const std::string longFile = std::string(argv[1]) + "/L-ref.flac";
    for (const bool own : {false, true}) {
        setOwnDecoderInUseForTesting(own);
        const std::string through = own ? "FFmpeg: " : "Qt: ";
        check(!QtMultimediaSilenceProbe(-60.0, 0).measure(longFile).has_value(), through + "no silence span in no time");
        check(!QtMultimediaAlignmentProbe(0).decoded(longFile), through + "nothing kept for alignment in no time");
        std::size_t fed = 0;
        check(!decodeMono(longFile, [&](const float *, std::size_t count, int) { fed += count; }, {}, 0),
              through + "no detail waveform in no time");
        fed = 0;
        std::size_t fedAtCancel = 0;
        bool cancelSeen = false;
        const bool decoded = decodeMono(
            longFile, [&](const float *, std::size_t count, int) { fed += count; },
            [&] {
                if (fed >= 20000 && !cancelSeen) {
                    cancelSeen = true;
                    fedAtCancel = fed;
                }
                return cancelSeen;
            });
        check(!decoded && cancelSeen, through + "a cancelled detail waveform is no waveform");
        check(fed == fedAtCancel && fed < 937125,
              through + "and nothing is fed after the cancel: " + std::to_string(fed) + " then, " + std::to_string(fedAtCancel)
                  + " at the cancel");
    }
    setOwnDecoderInUseForTesting(true);

    if (failures > 0) {
        std::cerr << failures << " check(s) FAILED over " << compared << " files\n";
        return 1;
    }
    std::cout << "ffmpeg_decoders_test: " << compared << " files, the same through both decoders\n";
    return 0;
}
