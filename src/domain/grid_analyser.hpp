// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>
#include <vector>

#include "domain/beat_grid.hpp"
#include "domain/pcm_source.hpp"
#include "musicanalyze/grid.hpp"

namespace seabass::domain
{

// What a beat grid analyser makes of a track: every beat it found, on the
// source's timeline (sample 0 at 0 ms), and how sure it is (0 to 1; an
// analyser that cannot say gives 0, and the benchmark reports it beside
// the score and never weighs by it). The analysis library's type, which
// the baseline analysers the benchmark compares it with fill as well. A
// source that failed (PcmSource::error) is reported in its `error`
// rather than as an empty grid.
using musicanalyze::GridAnalysis;

// A beat grid analyser over the PcmSource port, so it runs on generated
// samples in a test exactly as on a decoded file in the app.
class GridAnalyser
{
public:
    virtual ~GridAnalyser() = default;
    // A short name for reports.
    virtual std::string name() const = 0;
    virtual GridAnalysis analyse(PcmSource &source) = 0;
};

}  // namespace seabass::domain
