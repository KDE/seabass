// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/beat_grid.hpp"

#include <algorithm>
#include <cmath>

namespace seabass::domain
{

namespace
{
// A beat shorter than this is over 600 BPM: not music, a damaged grid.
constexpr double kShortestBeatMs = 100.0;
// No track has this many beats; a grid that implies more is not believed.
constexpr std::size_t kMostBeats = 100000;
}  // namespace

std::vector<Beat> beatsFromMarkers(std::vector<BeatGridMarker> markers, double durationMs)
{
    // A marker that is not a number pins nothing.
    markers.erase(std::remove_if(markers.begin(), markers.end(),
                                 [](const BeatGridMarker &marker) { return !std::isfinite(marker.timeMs); }),
                  markers.end());
    std::sort(markers.begin(), markers.end(),
              [](const BeatGridMarker &a, const BeatGridMarker &b) { return a.index < b.index; });

    std::vector<Beat> beats;
    for (std::size_t m = 0; m + 1 < markers.size(); ++m) {
        const BeatGridMarker &from = markers[m];
        const BeatGridMarker &to = markers[m + 1];
        if (to.index <= from.index) {
            continue;
        }
        // In 64 bits throughout: the indices are whatever a file says.
        const long long span = static_cast<long long>(to.index) - from.index;
        const double beatMs = (to.timeMs - from.timeMs) / static_cast<double>(span);
        if (!(beatMs >= kShortestBeatMs)) {
            continue;
        }
        // The closing marker's own beat belongs to the next span, except
        // at the very end where there is none.
        const bool lastSpan = m + 2 == markers.size();
        long long first = 0;
        long long last = lastSpan ? span : span - 1;
        // Only the beats inside the track are walked, worked out rather
        // than found by walking: a damaged pair of markers can span two
        // thousand million beats with a handful inside the track, and
        // walking them all to skip them froze the app for as long.
        first = std::max(first, static_cast<long long>(std::ceil((0.0 - from.timeMs) / beatMs)));
        if (durationMs > 0.0) {
            last = std::min(last, static_cast<long long>(std::floor((durationMs - from.timeMs) / beatMs)));
        }
        if (last - first + 1 > static_cast<long long>(kMostBeats - beats.size())) {
            return {};
        }
        for (long long step = first; step <= last; ++step) {
            const long long index = from.index + step;
            beats.push_back({from.timeMs + beatMs * static_cast<double>(step), static_cast<int>(((index % 4) + 4) % 4) + 1});
        }
    }
    return beats;
}

std::vector<Beat> beatsInOrder(std::vector<Beat> beats)
{
    std::vector<Beat> kept;
    kept.reserve(beats.size());
    for (const Beat &beat : beats) {
        if (!std::isfinite(beat.timeMs) || beat.timeMs < 0.0 || (!kept.empty() && beat.timeMs <= kept.back().timeMs)) {
            continue;
        }
        kept.push_back(beat);
    }
    return kept;
}

double positionAfterSkippingBeats(const std::vector<double> &beatTimesMs, double positionMs, int beats,
                                  double durationMs)
{
    double target = positionMs + beats * 625.0;
    if (beatTimesMs.size() > 1 && positionMs >= beatTimesMs.front() && positionMs <= beatTimesMs.back()) {
        const auto after = std::upper_bound(beatTimesMs.begin(), beatTimesMs.end(), positionMs);
        const auto here = static_cast<long>(after - beatTimesMs.begin()) - 1;
        const long there = std::clamp(here + beats, 0L, static_cast<long>(beatTimesMs.size()) - 1);
        target = beatTimesMs[static_cast<std::size_t>(there)] + (positionMs - beatTimesMs[static_cast<std::size_t>(here)]);
    }
    if (durationMs > 0.0) {
        target = std::min(target, durationMs - 1.0);
    }
    return std::max(0.0, target);
}

}  // namespace seabass::domain
