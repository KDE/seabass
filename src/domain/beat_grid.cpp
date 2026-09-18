// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/beat_grid.hpp"

#include <algorithm>

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
    std::sort(markers.begin(), markers.end(),
              [](const BeatGridMarker &a, const BeatGridMarker &b) { return a.index < b.index; });

    std::vector<Beat> beats;
    for (std::size_t m = 0; m + 1 < markers.size(); ++m) {
        const BeatGridMarker &from = markers[m];
        const BeatGridMarker &to = markers[m + 1];
        if (to.index <= from.index) {
            continue;
        }
        const double beatMs = (to.timeMs - from.timeMs) / static_cast<double>(to.index - from.index);
        if (beatMs < kShortestBeatMs) {
            continue;
        }
        // The closing marker's own beat belongs to the next span, except
        // at the very end where there is none.
        const bool lastSpan = m + 2 == markers.size();
        const int end = lastSpan ? to.index : to.index - 1;
        for (int index = from.index; index <= end; ++index) {
            const double timeMs = from.timeMs + beatMs * static_cast<double>(index - from.index);
            if (timeMs < 0.0 || (durationMs > 0.0 && timeMs > durationMs)) {
                continue;
            }
            if (beats.size() >= kMostBeats) {
                return {};
            }
            beats.push_back({timeMs, ((index % 4) + 4) % 4 + 1});
        }
    }
    return beats;
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
