// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <span>

#include "domain/grid_analyser.hpp"
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
    return musicanalyze::analyse(samples, sampleRateHz, range);
}

// The tempo the range setting stands for when nothing is set: the
// analyser's default.
constexpr TempoRange DefaultTempoRange{};

}  // namespace seabass::domain
