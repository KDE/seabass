// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "domain/grid_analyser.hpp"
#include "domain/pcm_source.hpp"
#include "domain/key_from_audio.hpp"
#include "domain/seabass_grid_analyser.hpp"
#include "domain/waveform_from_audio.hpp"
#include "musicanalyze/analysis.hpp"

namespace seabass::domain
{

// Everything Seabass's own analysis makes of a track's audio, from one
// set of samples (grid, tempo, key, waveform, and the revisions of the
// code that made each): the analysis library's result
// (lib/musicanalyze, analysis.hpp, where each part is written down).
using AudioAnalysis = musicanalyze::Analysis;

// The one call for whoever in Seabass has a decoded track and wants it
// analysed (the track import; later the page that analyses a library).
// `samples` are mono, at `sampleRateHz`, and are read and not kept. The
// range says which tempo is the beat where half or double would do as
// well: the default suits most music; drum and bass at its faster tempo
// wants about 88 to 175.
inline AudioAnalysis analyseAudio(std::span<const float> samples, int sampleRateHz, TempoRange range = {})
{
    return musicanalyze::analyze(samples, sampleRateHz, range);
}

// The same of a source, read to its end piece by piece and never held
// whole: a mix of two hours takes some hundreds of megabytes this way
// where its samples alone would take over a gigabyte (the numbers are in
// lib/musicanalyze/README.md). The result is the one the samples joined
// would give. The source's error, if it stopped on one, goes into
// `error` (when given), and the analysis is then of what was read: a
// caller must check it, since a track cut short by a decode error is not
// the whole track.
inline AudioAnalysis analyseAudio(PcmSource &source, TempoRange range = {}, std::string *error = nullptr)
{
    musicanalyze::Analyzer analyser(source.sampleRate(), range);
    std::vector<float> piece(65536);
    for (;;) {
        const std::size_t got = source.read(piece);
        if (got == 0) {
            break;
        }
        analyser.feed(std::span<const float>(piece.data(), got));
    }
    if (error != nullptr) {
        *error = source.error();
    }
    return analyser.finish();
}

// The tempo the range setting stands for when nothing is set: the
// analyser's default.
constexpr TempoRange DefaultTempoRange{};

}  // namespace seabass::domain
