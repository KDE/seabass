// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <span>

#include "domain/grid_analyser.hpp"
#include "domain/key_from_audio.hpp"
#include "domain/seabass_grid_analyser.hpp"
#include "domain/waveform_from_audio.hpp"

namespace seabass::domain
{

// Everything Seabass's own analysis makes of a track's audio, from one
// set of samples (grid, tempo, key, waveform): the one call for whoever has a decoded track and wants
// it analysed (the track import; later the page that analyses a library).
// What is analysed how is decided here, so a caller gets every
// improvement by building against a newer version and changes nothing.
struct AudioAnalysis
{
    // The beat grid, on the samples' timeline: sample 0 at 0 ms. Its
    // `error` says why there is none; no beats and no error is a track
    // with no beat the analyser could find (speech, ambience).
    // `followsTempo` marks a grid whose spacing changes; `confidence`
    // how much of the track the grid explains.
    GridAnalysis grid;
    // The grid's tempo as one number: a minute over the middle gap
    // between two beats, not rounded. 0 without a grid of two beats. On
    // a grid that follows the tempo it is the track's usual tempo, not
    // the tempo everywhere.
    double bpm = 0.0;
    // The musical key, where one could be read; keyName() spells it as
    // the libraries do ("Fm", "Db"). `margin` tells a clear key from a
    // close call.
    KeyAnalysis key;
    // 150 entries a second. Empty where the sample rate is too low to
    // measure one (WaveformLowestSampleRateHz); the grid does not depend
    // on it.
    AudioWaveform waveform;
};

// `samples` are mono, at `sampleRateHz`, and are read and not kept. The
// range says which tempo is the beat where half or double would do as
// well: the default suits most music; drum and bass at its faster tempo
// wants about 88 to 175.
AudioAnalysis analyseAudio(std::span<const float> samples, int sampleRateHz, TempoRange range = {});

// The tempo the range setting stands for when nothing is set: the
// analyser's default.
constexpr TempoRange DefaultTempoRange{};

}  // namespace seabass::domain
