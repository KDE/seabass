// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>
#include <vector>

#include "domain/beat_grid.hpp"
#include "domain/pcm_source.hpp"

namespace seabass::domain
{

// What a beat grid analyser makes of a track: every beat it found, on the
// source's timeline (sample 0 at 0 ms), and how sure it is.
struct GridAnalysis
{
    std::vector<Beat> beats;
    // 0 to 1. An analyser that cannot say gives 0; the benchmark reports
    // it beside the score and never weighs by it.
    double confidence = 0.0;
    // Empty when the analyser ran. A source that failed (PcmSource::error)
    // is reported here rather than as an empty grid, which would read as
    // "no beats in this track".
    std::string error;
};

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
