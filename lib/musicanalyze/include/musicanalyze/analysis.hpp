// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <span>

#include "musicanalyze/grid.hpp"
#include "musicanalyze/key.hpp"
#include "musicanalyze/version.hpp"
#include "musicanalyze/waveform.hpp"

namespace musicanalyze
{

// Everything the library makes of a track's audio, from one set of
// samples (grid, tempo, key, waveform): the one call for whoever has a
// decoded track and wants it analyzed. What is analyzed how is decided
// here, so a caller gets every improvement by building against a newer
// version and changes nothing.
struct Analysis
{
    // The beat grid, on the samples' timeline: sample 0 at 0 ms. Its
    // `error` says why there is none; no beats and no error is a track
    // with no beat the analyzer could find (speech, ambience).
    // `followsTempo` marks a grid whose spacing changes; `confidence`
    // how much of the track the grid explains.
    GridAnalysis grid;
    // The grid's tempo as one number, usualBpm() of its beats: 0
    // without a grid of two beats, and on a grid that follows the tempo
    // the track's usual tempo, not the tempo everywhere.
    double bpm = 0.0;
    // The musical key, where one could be read; keyName() spells it as
    // DJ libraries do ("Fm", "Db"). `margin` tells a clear key from a
    // close call.
    KeyAnalysis key;
    // 150 entries a second. Empty where the sample rate is too low to
    // measure one (WaveformLowestSampleRateHz); the grid does not depend
    // on it.
    AudioWaveform waveform;
    // What made each part (version.hpp): to be kept beside the result
    // by whoever stores it.
    Revisions revisions;
};

// `samples` are mono, full scale 1.0, at `sampleRateHz`, and are read
// and not kept. The range says which tempo is the beat where half or
// double would do as well: the default suits most music; drum and bass
// at its faster tempo wants about 88 to 175.
Analysis analyze(std::span<const float> samples, int sampleRateHz, TempoRange range = {});

// The same of audio that comes in pieces, a decoder's buffers say: feed()
// them in order, of any length, then finish(). The result is the one
// analyze() gives for the pieces joined (which is this, fed once). What
// is held meanwhile is a few seconds of audio and what was heard so far
// (about 45 MB, and 4.3 MB a minute of music), and never the track: a mix
// of two hours takes under 600 MB, where its samples alone are 1.3 GB.
class Analyzer
{
public:
    explicit Analyzer(int sampleRateHz, TempoRange range = {});

    void feed(std::span<const float> samples);
    // Everything made of what was fed; the analyzer is spent after it.
    Analysis finish();

private:
    BeatGridAnalyzer m_grid;
    KeyAnalyzer m_key;
    WaveformMeter m_waveform;
};

}  // namespace musicanalyze
