// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Two things. beatsFromMarkers() on grids whose beats can be counted by
// hand; and both readers against the committed fixture, where the right
// answer is not known beat by beat but what a beat grid IS is: beats in
// order, a musical tempo apart, counting one-two-three-four.
//
// usage: beat_grid_test <fixture root holding rekordbox/ and engine/>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <djinterop/djinterop.hpp>

#include "domain/beat_grid.hpp"
#include "infrastructure/engine/libdjinterop_beat_grid_reader.hpp"
#include "infrastructure/rekordbox/anlz_path_index.hpp"
#include "infrastructure/rekordbox/rekordbox_beat_grid_reader.hpp"

using namespace seabass;

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

// What any real grid looks like. Returns false, saying why, if not.
bool looksLikeAGrid(const std::vector<domain::Beat> &beats, const std::string &who)
{
    bool ok = true;
    const auto fail = [&](const std::string &why) {
        std::cerr << "FAILED: " << who << ": " << why << "\n";
        ++failures;
        ok = false;
    };
    if (beats.size() < 16) {
        fail("only " + std::to_string(beats.size()) + " beats");
        return false;
    }
    std::vector<double> lengths;
    for (std::size_t i = 1; i < beats.size(); ++i) {
        if (beats[i].timeMs <= beats[i - 1].timeMs) {
            fail("beat " + std::to_string(i) + " is not after the one before it");
            return false;
        }
        lengths.push_back(beats[i].timeMs - beats[i - 1].timeMs);
        const int expected = beats[i - 1].beatInBar % 4 + 1;
        if (beats[i - 1].beatInBar != 0 && beats[i].beatInBar != expected) {
            fail("beat " + std::to_string(i) + " counts " + std::to_string(beats[i].beatInBar) + " after "
                 + std::to_string(beats[i - 1].beatInBar));
            return false;
        }
    }
    std::sort(lengths.begin(), lengths.end());
    const double median = lengths[lengths.size() / 2];
    if (median < 250.0 || median > 1200.0) {  // 240 down to 50 BPM
        fail("a median beat of " + std::to_string(median) + " ms is no tempo");
    }
    if (beats.front().timeMs < 0.0) {
        fail("a beat before the track starts");
    }
    return ok;
}
}  // namespace

int main(int argc, char **argv)
{
    // Engine's usual shape: one marker before the start, one past the
    // end, 500 ms to the beat. Index 0 lands at 100 ms.
    {
        const auto beats = domain::beatsFromMarkers({{-4, -1900.0}, {396, 198100.0}}, 180000.0);
        check(!beats.empty() && std::abs(beats.front().timeMs - 100.0) < 1e-6, "the first beat inside the track is index 0, at 100 ms");
        check(!beats.empty() && beats.front().beatInBar == 1, "and index 0 is a downbeat");
        check(beats.size() == 360, "100 ms to 179600 ms at 500 ms is 360 beats, got " + std::to_string(beats.size()));
        check(beats.size() > 5 && beats[1].beatInBar == 2 && beats[4].beatInBar == 1, "counting one two three four one");
        check(!beats.empty() && beats.back().timeMs <= 180000.0, "and none past the end");
    }
    // A tempo change: two spans, the middle marker's beat counted once.
    {
        const auto beats = domain::beatsFromMarkers({{0, 0.0}, {4, 2000.0}, {8, 3000.0}}, 0.0);
        check(beats.size() == 9, "indices 0 to 8 are nine beats, got " + std::to_string(beats.size()));
        check(beats.size() == 9 && std::abs(beats[4].timeMs - 2000.0) < 1e-6 && std::abs(beats[5].timeMs - 2250.0) < 1e-6,
              "500 ms beats, then 250 ms ones from the marker on");
    }
    // Markers out of order are the same grid.
    {
        const auto beats = domain::beatsFromMarkers({{8, 4000.0}, {0, 0.0}}, 0.0);
        check(beats.size() == 9 && beats.front().timeMs == 0.0, "markers are sorted before use");
    }
    // Nothing to go on, or nonsense, is no grid -- not a crash, not a guess.
    {
        check(domain::beatsFromMarkers({}, 1000.0).empty(), "no markers, no beats");
        check(domain::beatsFromMarkers({{0, 0.0}}, 1000.0).empty(), "one marker pins a beat but not a tempo");
        check(domain::beatsFromMarkers({{0, 0.0}, {1000, 1000.0}}, 0.0).empty(), "a 1 ms beat is a damaged grid");
        check(domain::beatsFromMarkers({{3, 0.0}, {3, 500.0}}, 0.0).empty(), "two markers on one index span nothing");
    }

    if (argc < 2) {
        std::cerr << "usage: beat_grid_test <fixture root>\n";
        return 2;
    }
    const std::string root = argv[1];

    // rekordbox: every track the fixture has an analysis file for.
    {
        const std::string pioneerRoot = root + "/rekordbox";
        infrastructure::rekordbox::AnlzPathIndex index(pioneerRoot);
        int withGrid = 0;
        int asked = 0;
        for (uint32_t id = 1; id < 5000 && asked < 25; ++id) {
            if (!index.pathFor(id)) {
                continue;
            }
            ++asked;
            const auto beats = infrastructure::rekordbox::readBeatGrid(pioneerRoot, std::to_string(id));
            if (!beats.empty() && looksLikeAGrid(beats, "rekordbox track " + std::to_string(id))) {
                ++withGrid;
                check(beats.front().beatInBar >= 1, "rekordbox says where in the bar every beat is");
            }
        }
        check(asked == 25, "the fixture has 25 analysed rekordbox tracks to ask about, found " + std::to_string(asked));
        check(withGrid >= 20, "and nearly all of them have a grid, got " + std::to_string(withGrid));
        check(infrastructure::rekordbox::readBeatGrid(pioneerRoot, "4000000").empty(), "an id the library has not is no grid");
        check(infrastructure::rekordbox::readBeatGrid(pioneerRoot, "not a number").empty(), "nor is nonsense");
        check(infrastructure::rekordbox::readBeatGrid("/nonexistent/PIONEER", "1").empty(), "nor a library that is not there");
    }

    // Engine: every track of the fixture that has markers at all. Some
    // are not analysed, and have none; those are rightly no grid. But a
    // track WITH markers must come out with beats -- including the ones
    // whose sample rate libdjinterop cannot read, which is a fifth of
    // them here and was every one of them coming back empty.
    {
        const std::string enginePath = root + "/engine";
        int withMarkers = 0;
        int withGrid = 0;
        int rateUnreadable = 0;
        auto db = djinterop::engine::load_database(enginePath);
        for (const auto &track : db.tracks()) {
            if (withMarkers == 40) {
                break;
            }
            const auto beats = infrastructure::engine::readBeatGrid(enginePath, std::to_string(track.id()));
            if (track.beatgrid().size() < 2) {
                check(beats.empty(), "a track without markers has no grid");
                continue;
            }
            ++withMarkers;
            try {
                (void)track.sample_rate();
            } catch (const std::exception &) {
                ++rateUnreadable;
            }
            if (!beats.empty() && looksLikeAGrid(beats, "Engine track " + std::to_string(track.id()))) {
                ++withGrid;
            }
        }
        check(withMarkers == 40, "the fixture has 40 Engine tracks with markers, found " + std::to_string(withMarkers));
        check(withGrid == withMarkers, "and every one of them gets its grid, got " + std::to_string(withGrid));
        check(rateUnreadable > 0, "some of them by working the sample rate out -- or this test no longer covers that");
        check(infrastructure::engine::readBeatGrid(enginePath, "4000000").empty(), "an id the library has not is no grid");
        check(infrastructure::engine::readBeatGrid("/nonexistent/Engine Library", "1").empty(), "nor a library that is not there");
    }

    if (failures == 0) {
        std::cout << "beat_grid_test: all cases passed\n";
    }
    return failures == 0 ? 0 : 1;
}
