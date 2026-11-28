// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The metronome window's arithmetic: which beat a moment follows, how
// far it is to the next, and a track's loudness cut into columns.

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

#include "domain/beat_timeline.hpp"

using namespace seabass::domain;

namespace
{

bool same(double a, double b)
{
    return std::abs(a - b) < 1e-9;
}

}  // namespace

int main()
{
    const std::vector<double> beats{100.0, 600.0, 1100.0, 1700.0};

    // The beat a moment follows: none before the first, a beat's own
    // moment belongs to it, the last one goes on for ever.
    assert(beatAt(beats, 0.0) == -1);
    assert(beatAt(beats, 99.999) == -1);
    assert(beatAt(beats, 100.0) == 0);
    assert(beatAt(beats, 599.0) == 0);
    assert(beatAt(beats, 600.0) == 1);
    assert(beatAt(beats, 5000.0) == 3);
    assert(beatAt({}, 5.0) == -1);

    // The phase: 0 on a beat, by the length of THIS gap (the third is
    // 600 ms long, not 500), and none where a beat is missing on a side.
    assert(same(beatPhaseAt(beats, 100.0), 0.0));
    assert(same(beatPhaseAt(beats, 350.0), 0.5));
    assert(same(beatPhaseAt(beats, 1400.0), 0.5));
    assert(same(beatPhaseAt(beats, 50.0), -1.0));
    assert(same(beatPhaseAt(beats, 1700.0), -1.0));
    assert(same(beatPhaseAt(beats, 9000.0), -1.0));
    assert(same(beatPhaseAt(std::vector<double>{100.0}, 100.0), -1.0));
    // Two beats at one place divide by nothing.
    assert(same(beatPhaseAt(std::vector<double>{100.0, 100.0, 200.0}, 100.0), 0.0));

    // Loudness: ten levels a second, so each is 100 ms.
    const std::vector<float> levels{0.1f, 0.9f, 0.2f, 0.3f, 0.8f, 0.1f};
    // Two columns over the whole: the loudest of each half.
    {
        const std::vector<float> got = loudestPerColumn(levels, 10.0, 0.0, 600.0, 2);
        assert(got.size() == 2 && same(got[0], 0.9f) && same(got[1], 0.8f));
    }
    // More columns than levels: every column still shows the level it
    // lies in, none comes out empty.
    {
        const std::vector<float> got = loudestPerColumn(levels, 10.0, 0.0, 200.0, 8);
        assert(got.size() == 8);
        for (int i = 0; i < 4; ++i) {
            assert(same(got[static_cast<std::size_t>(i)], 0.1f));
            assert(same(got[static_cast<std::size_t>(i + 4)], 0.9f));
        }
    }
    // A stretch that begins before the track and ends after it: silence
    // outside, the track inside.
    {
        const std::vector<float> got = loudestPerColumn(levels, 10.0, -200.0, 800.0, 10);
        assert(got.size() == 10);
        assert(same(got[0], 0.0f) && same(got[1], 0.0f) && same(got[2], 0.1f) && same(got[3], 0.9f));
        assert(same(got[7], 0.1f) && same(got[8], 0.0f) && same(got[9], 0.0f));
    }
    // Nothing to draw into, or no stretch: nothing, and no division.
    assert(loudestPerColumn(levels, 10.0, 0.0, 600.0, 0).empty());
    assert(loudestPerColumn(levels, 10.0, 600.0, 600.0, 4).empty());
    assert(loudestPerColumn(levels, 0.0, 0.0, 600.0, 4).empty());
    assert(loudestPerColumn({}, 10.0, 0.0, 600.0, 3) == std::vector<float>(3, 0.0f));
    // Ends that are no numbers give nothing; ends far beyond the track
    // give silence, not a whole number that does not exist.
    const double infinity = std::numeric_limits<double>::infinity();
    assert(loudestPerColumn(levels, 10.0, -infinity, 600.0, 4).empty());
    assert(loudestPerColumn(levels, 10.0, 0.0, std::nan(""), 4).empty());
    {
        const std::vector<float> got = loudestPerColumn(levels, 10.0, -1e300, 1e300, 2);
        assert(got.size() == 2 && same(got[0], 0.0f) && same(got[1], 0.9f));
    }
    assert(loudestPerColumn(levels, 10.0, 1e300, 1.5e300, 2) == std::vector<float>(2, 0.0f));

    std::cout << "beat_timeline_test: ok\n";
    return 0;
}
