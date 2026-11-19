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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <djinterop/djinterop.hpp>

#include <sqlite3.h>
#include "infrastructure/paths/utf8_path.hpp"
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

// Markers back to beats: every source beat must come back within a
// millisecond and on the same place in the bar, and whatever else comes
// back lies before the first source beat or after the last.
void roundTrips(const std::vector<domain::Beat> &source, const std::vector<domain::BeatGridMarker> &markers,
                double durationMs, const std::string &who)
{
    const auto beats = domain::beatsFromMarkers(markers, durationMs);
    std::size_t at = 0;
    while (at < beats.size() && beats[at].timeMs < source.front().timeMs - 1.0) {
        ++at;
    }
    if (beats.size() < at + source.size()) {
        check(false, who + ": " + std::to_string(beats.size()) + " beats back, too few for " + std::to_string(source.size()));
        return;
    }
    for (std::size_t i = 0; i < source.size(); ++i) {
        const domain::Beat &back = beats[at + i];
        if (std::abs(back.timeMs - source[i].timeMs) > 1.0 || back.beatInBar != source[i].beatInBar) {
            check(false, who + ": source beat " + std::to_string(i) + " at " + std::to_string(source[i].timeMs) + " ("
                             + std::to_string(source[i].beatInBar) + ") came back at " + std::to_string(back.timeMs) + " ("
                             + std::to_string(back.beatInBar) + ")");
            return;
        }
    }
    for (std::size_t i = at + source.size(); i < beats.size(); ++i) {
        if (beats[i].timeMs <= source.back().timeMs) {
            check(false, who + ": an extra beat inside the source grid");
            return;
        }
    }
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

    // Damaged markers. These used to be walked beat by beat: two thousand
    // million of them, to keep the few inside the track.
    {
        const auto started = std::chrono::steady_clock::now();
        const auto beats = domain::beatsFromMarkers({{0, 0.0}, {2000000000, 3.0e11}}, 60000.0);
        const auto took = std::chrono::steady_clock::now() - started;
        check(beats.size() == 401, "150 ms beats in a minute are 401, got " + std::to_string(beats.size()));
        check(took < std::chrono::milliseconds(200), "and they are worked out, not walked to");
        check(domain::beatsFromMarkers({{0, 0.0}, {2000000000, 3.0e11}}, 0.0).empty(), "with no end known that is more beats than any track has: no grid");
        check(domain::beatsFromMarkers({{2147483000, 0.0}, {2147483647, 323500.0}}, 0.0).size() == 648, "indices at the very top of the range do not overflow");
        const double nan = std::nan("");
        check(domain::beatsFromMarkers({{0, nan}, {8, 4000.0}}, 0.0).empty(), "a marker that is not a number pins nothing");
        check(domain::beatsFromMarkers({{0, 0.0}, {8, 4000.0}, {16, nan}}, 0.0).size() == 9, "and the good ones still do");
    }
    // A grid as a file has it, made fit to keep time by.
    {
        const auto beats = domain::beatsInOrder({{-5.0, 4}, {100.0, 1}, {600.0, 2}, {600.0, 3}, {std::nan(""), 4}, {400.0, 1}, {1100.0, 2}});
        check(beats.size() == 3 && beats[0].timeMs == 100.0 && beats[1].timeMs == 600.0 && beats[2].timeMs == 1100.0,
              "what is before the start, not a number, or not after the beat before it is dropped");
    }

    // A jump of whole beats lands as far into the beat as it left.
    {
        std::vector<double> grid;
        for (int i = 0; i < 100; ++i) {
            grid.push_back(1000.0 + i * 500.0);
        }
        using domain::positionAfterSkippingBeats;
        check(positionAfterSkippingBeats(grid, 5125.0, 4, 60000.0) == 7125.0, "four beats on from 125 ms into a beat is 125 ms into the fourth after");
        check(positionAfterSkippingBeats(grid, 5125.0, -4, 60000.0) == 3125.0, "and four back likewise");
        check(positionAfterSkippingBeats(grid, 1600.0, -4, 60000.0) == 1100.0, "back past the first beat stops in the first beat");
        check(positionAfterSkippingBeats(grid, 400.0, 4, 60000.0) == 2900.0, "before the grid starts a beat is 625 ms");
        check(positionAfterSkippingBeats({}, 10000.0, -4, 60000.0) == 7500.0, "and so it is with no grid at all");
        check(positionAfterSkippingBeats({}, 1000.0, -4, 60000.0) == 0.0, "never before the start");
        check(positionAfterSkippingBeats({}, 59000.0, 4, 60000.0) == 59999.0, "nor onto the very end, which a player takes for the track ending");
        // A tempo change between here and there: the offset into the beat is kept in milliseconds.
        std::vector<double> twoTempi = {0.0, 500.0, 1000.0, 1250.0, 1500.0, 1750.0};
        check(positionAfterSkippingBeats(twoTempi, 600.0, 3, 0.0) == 1600.0, "across a tempo change it still lands in the beat it should");
    }

    // markersFromBeats(): a rekordbox grid, every beat in whole
    // milliseconds, as Engine markers. Back through beatsFromMarkers() it
    // must be the same grid.
    {
        // 128 BPM from 37 ms, the first beat the third of its bar.
        std::vector<domain::Beat> source;
        for (int i = 0; i < 400; ++i) {
            source.push_back({std::round(37.0 + i * 468.75), (2 + i) % 4 + 1});
        }
        const double duration = 37.0 + 400 * 468.75;
        const auto converted = domain::markersFromBeats(source, duration);
        check(converted.markers.size() == 2, "a constant tempo is two markers, got " + std::to_string(converted.markers.size()));
        check(converted.barBreaks == 0, "and no bar breaks");
        check(!converted.markers.empty() && converted.markers.front().timeMs <= 0.0, "the first marker is at or before the start");
        check(!converted.markers.empty() && converted.markers.back().timeMs >= duration, "the last at or past the end");
        roundTrips(source, converted.markers, duration, "constant tempo");
    }
    {
        // 120 BPM, then 126 from beat 200 on.
        std::vector<domain::Beat> source;
        double t = 500.0;
        for (int i = 0; i < 400; ++i) {
            source.push_back({std::round(t), i % 4 + 1});
            t += i < 199 ? 500.0 : 60000.0 / 126.0;
        }
        const auto converted = domain::markersFromBeats(source, t);
        check(converted.markers.size() == 3, "one tempo change is three markers, got " + std::to_string(converted.markers.size()));
        check(converted.markers.size() == 3 && converted.markers[1].timeMs == source[199].timeMs,
              "the middle one on the beat where the tempo changes");
        roundTrips(source, converted.markers, t, "tempo change");
    }
    {
        // Each beat 0.05 ms longer than the last: no one line holds it.
        std::vector<domain::Beat> source;
        double t = 0.0;
        double beat = 450.0;
        for (int i = 0; i < 400; ++i) {
            source.push_back({t, i % 4 + 1});
            t += beat;
            beat += 0.05;
        }
        const auto converted = domain::markersFromBeats(source, 0.0);
        check(converted.markers.size() > 3, "a drifting tempo takes several spans, got " + std::to_string(converted.markers.size()));
        roundTrips(source, converted.markers, 0.0, "drift");
    }
    {
        // 1 2 3 4 1 2 3 1 2 3 4 1: the bar restarts after a 3.
        const std::vector<int> bars = {1, 2, 3, 4, 1, 2, 3, 1, 2, 3, 4, 1};
        std::vector<domain::Beat> source;
        for (std::size_t i = 0; i < bars.size(); ++i) {
            source.push_back({1000.0 + 500.0 * static_cast<double>(i), bars[i]});
        }
        const auto converted = domain::markersFromBeats(source, 0.0);
        check(converted.barBreaks == 1, "one bar break, got " + std::to_string(converted.barBreaks));
        check(converted.markers.size() == 2 && converted.markers.back().index - converted.markers.front().index == 13,
              "and the indices run on through it: two to the start, eleven to the last beat");
        const auto beats = domain::beatsFromMarkers(converted.markers, 0.0);
        check(beats.size() == 14 && beats[2].beatInBar == 1 && beats[9].beatInBar == 4,
              "the first bar position wins: the beat after the break counts 4");
    }
    {
        std::vector<domain::Beat> source;
        for (int i = 0; i < 16; ++i) {
            source.push_back({250.0 + 500.0 * i, 0});
        }
        const auto converted = domain::markersFromBeats(source, 0.0);
        const auto beats = domain::beatsFromMarkers(converted.markers, 0.0);
        check(converted.barBreaks == 0, "no bar information is no bar break");
        check(beats.size() == 16 && beats[0].timeMs == 250.0 && beats[0].beatInBar == 1,
              "and the first source beat is index 0, a downbeat");
        check(!converted.markers.empty() && converted.markers.back().timeMs == 7750.0,
              "with no duration the last marker is on the last beat");
    }
    // Nothing to go on, or nonsense: no grid or a sane one, never a crash,
    // never a number that is not one.
    {
        const auto finite = [](const domain::MarkersFromBeats &converted) {
            return std::all_of(converted.markers.begin(), converted.markers.end(),
                               [](const domain::BeatGridMarker &marker) { return std::isfinite(marker.timeMs); });
        };
        const double nan = std::nan("");
        const double inf = std::numeric_limits<double>::infinity();
        check(domain::markersFromBeats({}, 1000.0).markers.empty(), "no beats, no markers");
        check(domain::markersFromBeats({{100.0, 1}}, 1000.0).markers.empty(), "one beat is no tempo");
        check(domain::markersFromBeats({{100.0, 1}, {110.0, 2}}, 1000.0).markers.empty(), "two beats 10 ms apart are no tempo");
        check(domain::markersFromBeats({{nan, 1}, {inf, 2}, {-inf, 3}}, 1000.0).markers.empty(), "beats that are no numbers are no grid");
        const auto mixed = domain::markersFromBeats({{nan, 1}, {0.0, 1}, {500.0, 2}, {inf, 3}, {1000.0, 3}}, nan);
        check(mixed.markers.size() == 2 && finite(mixed), "the good beats of a damaged grid still make one");
        const auto endless = domain::markersFromBeats({{0.0, 1}, {500.0, 2}, {1000.0, 3}}, inf);
        check(endless.markers.size() == 2 && finite(endless), "an endless track is no endless grid");
        const auto far = domain::markersFromBeats({{1.0e15, 1}, {1.0e15 + 500.0, 2}}, 1.0e16);
        check(finite(far) && std::all_of(far.markers.begin(), far.markers.end(),
                                         [](const domain::BeatGridMarker &marker) { return std::abs(marker.index) < 1000000; }),
              "a grid absurdly far into the track does not overflow its indices");
    }
    // A whole track's worth of beats, a hundred thousand of them, in one go.
    {
        std::vector<domain::Beat> source;
        for (int i = 0; i < 100000; ++i) {
            source.push_back({std::round(20.0 + i * 468.75), i % 4 + 1});
        }
        const auto started = std::chrono::steady_clock::now();
        const auto converted = domain::markersFromBeats(source, 0.0);
        const auto took = std::chrono::steady_clock::now() - started;
        check(converted.markers.size() == 2, "a hundred thousand beats of one tempo are two markers");
        check(took < std::chrono::milliseconds(200), "and they are worked out in one pass, not one per beat");
    }
    // The same hundred thousand beats, each 0.9 ms off a 500 ms line, one
    // early and the next late: jitter at nearly the whole allowance, the
    // input the span search's look-ahead limit was written against. Same
    // budget as above. (Raising kFurthestLookAhead does not make this
    // slower: each span here runs out of possible lengths two beats in,
    // so the search is one pass with or without the limit.)
    {
        std::vector<domain::Beat> source;
        for (int i = 0; i < 100000; ++i) {
            source.push_back({1000.0 + i * 500.0 + (i % 2 == 0 ? 0.9 : -0.9), i % 4 + 1});
        }
        const auto started = std::chrono::steady_clock::now();
        const auto converted = domain::markersFromBeats(source, 0.0);
        const auto took = std::chrono::steady_clock::now() - started;
        check(converted.markers.size() >= 2, "beats jittering on one line still make a grid");
        check(took < std::chrono::milliseconds(200),
              "and jitter does not make the span search quadratic, took "
                  + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()) + " ms");
    }

    // engineNumbered(): markers put the way Engine numbers its own, the
    // first one beat -4 before the start, beat 0 the first downbeat in it.
    {
        const auto sameBeats = [](const std::vector<domain::BeatGridMarker> &before,
                                  const std::vector<domain::BeatGridMarker> &after, double durationMs) {
            const auto a = domain::beatsFromMarkers(before, durationMs);
            const auto b = domain::beatsFromMarkers(after, durationMs);
            if (a.size() != b.size() || a.empty()) {
                return false;
            }
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (std::abs(a[i].timeMs - b[i].timeMs) > 1e-6 || a[i].beatInBar != b[i].beatInBar) {
                    return false;
                }
            }
            return true;
        };
        const auto engineShaped = [](const std::vector<domain::BeatGridMarker> &markers, double beatMs) {
            return markers.size() >= 2 && markers.front().index == -4 && markers.front().timeMs < 0.0
                && markers[1].index > 0 && markers.front().timeMs + 4 * beatMs >= 0.0
                && markers.front().timeMs + 4 * beatMs < 4 * beatMs;
        };
        // 128 BPM from 37 ms, the first beat the third of its bar: index 0
        // is the second source beat's bar's next downbeat, two beats in.
        {
            std::vector<domain::Beat> source;
            for (int i = 0; i < 400; ++i) {
                source.push_back({std::round(37.0 + i * 468.75), (2 + i) % 4 + 1});
            }
            const double duration = 37.0 + 400 * 468.75;
            const auto markers = domain::markersFromBeats(source, duration).markers;
            const auto numbered = domain::engineNumbered(markers);
            const double beatMs = (markers[1].timeMs - markers[0].timeMs) / (markers[1].index - markers[0].index);
            check(engineShaped(numbered, beatMs), "a constant tempo is numbered from -4, before the start");
            check(numbered.size() == markers.size(), "and keeps its marker count");
            check(sameBeats(markers, numbered, duration), "with every beat where it was and on the same place in the bar");
            roundTrips(source, numbered, duration, "constant tempo, Engine's numbering");
            const auto beats = domain::beatsFromMarkers(numbered, duration);
            check(!beats.empty() && std::abs(beats[2].timeMs - source[2].timeMs) <= 1.0 && beats[2].beatInBar == 1,
                  "index 0 is the first downbeat, the third source beat");
        }
        // A tempo change: only the first marker moves, the rest are
        // renumbered by the same amount.
        {
            std::vector<domain::Beat> source;
            double t = 500.0;
            for (int i = 0; i < 400; ++i) {
                source.push_back({std::round(t), i % 4 + 1});
                t += i < 199 ? 500.0 : 60000.0 / 126.0;
            }
            const auto markers = domain::markersFromBeats(source, t).markers;
            const auto numbered = domain::engineNumbered(markers);
            check(engineShaped(numbered, 500.0), "a grid with a tempo change is numbered from -4 too");
            check(numbered.size() == 3 && numbered[1].timeMs == markers[1].timeMs && numbered[2].timeMs == markers[2].timeMs,
                  "its later markers stay where they were");
            check(sameBeats(markers, numbered, t), "and every beat keeps its time and place in the bar");
            roundTrips(source, numbered, t, "tempo change, Engine's numbering");
        }
        // A grid starting exactly at 0 on a downbeat: that beat is index 0.
        {
            const std::vector<domain::BeatGridMarker> markers = {{0, 0.0}, {400, 200000.0}};
            const auto numbered = domain::engineNumbered(markers);
            check(numbered.size() == 2 && numbered[0].index == -4 && numbered[0].timeMs == -2000.0 && numbered[1].index == 400,
                  "a downbeat at 0 is index 0, the first marker four beats before it");
            check(sameBeats(markers, numbered, 200000.0), "and nothing moves");
        }
        // Marker indices far from 0 and a first marker far before the start.
        {
            const std::vector<domain::BeatGridMarker> markers = {{1001, -10250.0}, {1500, 239250.0}};
            const auto numbered = domain::engineNumbered(markers);
            check(engineShaped(numbered, 500.0), "far off indices are brought to Engine's numbering");
            check(sameBeats(markers, numbered, 239000.0), "with every beat in place");
        }
        // Left as they were.
        {
            check(domain::engineNumbered({}).empty(), "no markers stay none");
            const std::vector<domain::BeatGridMarker> one = {{3, -100.0}};
            check(domain::engineNumbered(one).size() == 1 && domain::engineNumbered(one)[0].index == 3, "one marker stays as it was");
            // The first span is two beats long and ends 2000 ms before the
            // start: the marker before the first downbeat in the track
            // would be index 4, past the second marker, off the first
            // span's line.
            const std::vector<domain::BeatGridMarker> early = {{0, -3000.0}, {2, -2000.0}, {400, 197000.0}};
            const auto earlyOut = domain::engineNumbered(early);
            check(earlyOut.size() == 3 && earlyOut[0].index == 0 && earlyOut[1].index == 2 && earlyOut[2].index == 400,
                  "a tempo change inside the first bar leaves the markers as they were");
            const std::vector<domain::BeatGridMarker> notANumber = {{0, std::nan("")}, {400, 200000.0}};
            check(domain::engineNumbered(notANumber)[0].index == 0, "a marker that is no number leaves them as they were");
            const std::vector<domain::BeatGridMarker> backwards = {{10, 0.0}, {5, 2500.0}};
            check(domain::engineNumbered(backwards)[0].index == 10, "markers out of order are left as they were");
        }
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
        // The rate-from-markers path is no longer exercised by the fixture
        // itself: its "unreadable" records were Engine 3.x 68-byte ones,
        // which libdjinterop reads now. Covered on a scratch copy instead,
        // with one marker track's data record made undecodable the way a
        // damaged stick has it -- the grid must still come out.
        {
            namespace fs = std::filesystem;
            const fs::path scratch = fs::temp_directory_path()
                / seabass::pathFromUtf8("seabass-beat-grid-" + std::to_string(
                      std::chrono::steady_clock::now().time_since_epoch().count()));
            fs::remove_all(scratch);
            fs::copy(seabass::pathFromUtf8(enginePath), scratch, fs::copy_options::recursive);
            int64_t victim = 0;
            for (const auto &track : db.tracks()) {
                if (track.beatgrid().size() >= 2) {
                    victim = track.id();
                    break;
                }
            }
            sqlite3 *handle = nullptr;
            check(sqlite3_open(seabass::pathToUtf8(scratch / "Database2" / "m.db").c_str(), &handle) == SQLITE_OK,
                  "the scratch copy opens");
            const std::string damage = "UPDATE PerformanceData SET trackData = X'00' WHERE trackId = " + std::to_string(victim);
            check(sqlite3_exec(handle, damage.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK, "its record is damaged");
            sqlite3_close(handle);
            auto damagedDb = djinterop::engine::load_database(seabass::pathToUtf8(scratch));
            bool unreadable = false;
            try {
                (void)damagedDb.track_by_id(victim)->sample_rate();
            } catch (const std::exception &) {
                unreadable = true;
            }
            check(unreadable, "libdjinterop cannot read the damaged record's rate");
            const auto beats = infrastructure::engine::readBeatGrid(seabass::pathToUtf8(scratch), std::to_string(victim));
            check(!beats.empty() && looksLikeAGrid(beats, "damaged Engine track " + std::to_string(victim)),
                  "the grid still comes out, its rate worked out from the markers");
            std::error_code ec;
            fs::remove_all(scratch, ec);
        }
        (void)rateUnreadable;
        check(infrastructure::engine::readBeatGrid(enginePath, "4000000").empty(), "an id the library has not is no grid");
        check(infrastructure::engine::readBeatGrid("/nonexistent/Engine Library", "1").empty(), "nor a library that is not there");
    }

    if (failures == 0) {
        std::cout << "beat_grid_test: all cases passed\n";
    }
    return failures == 0 ? 0 : 1;
}
