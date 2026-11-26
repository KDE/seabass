// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "domain/grid_analyser.hpp"
#include "musicanalyze/grid.hpp"

namespace seabass::domain
{

// The tempos an analysis may give out; musicanalyze/grid.hpp says what a
// range means and how one is made valid.
using musicanalyze::TempoRange;

// Seabass's own beat grid analyser behind the GridAnalyser port, for the
// benchmark and the tests, which run every analyser through that port.
// The analysis itself is the library's (lib/musicanalyze, grid.hpp, where
// what it does is written down); this reads a source to its end and
// hands it over piece by piece, so the track is never held.
class SeabassGridAnalyser : public GridAnalyser
{
public:
    explicit SeabassGridAnalyser(TempoRange range = {}) : m_range(musicanalyze::usableTempoRange(range)) {}

    static constexpr double FollowedGridEarlierMs = musicanalyze::FollowedGridEarlierMs;
    static constexpr double StraightGridLaterMs = musicanalyze::StraightGridLaterMs;

    std::string name() const override { return "seabass"; }

    GridAnalysis analyse(PcmSource &source) override
    {
        musicanalyze::BeatGridAnalyser analyser(source.sampleRate(), m_range);
        std::vector<float> piece(65536);
        for (;;) {
            const std::size_t got = source.read(piece);
            if (got == 0) {
                break;
            }
            analyser.feed(std::span<const float>(piece.data(), got));
        }
        if (!source.error().empty()) {
            GridAnalysis failed;
            failed.error = source.error();
            return failed;
        }
        return analyser.finish();
    }

    // The same analysis of samples the caller holds already (mono, on the
    // timeline the beats come back on). No samples, or no sample rate:
    // the error says so.
    GridAnalysis analyseSamples(std::span<const float> samples, int sampleRateHz) const
    {
        return musicanalyze::gridFromSamples(samples, sampleRateHz, m_range);
    }

    // The range in use: the one given, made valid.
    TempoRange tempoRange() const { return m_range; }

private:
    TempoRange m_range;
};

}  // namespace seabass::domain
