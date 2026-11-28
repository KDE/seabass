// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// A moment of a track against its beats, and its loudness cut into
// columns: what something that moves or draws in time with the music
// asks (the metronome window). `beatsMs` ascend.
namespace seabass::domain
{

// Which beat `ms` follows, counted from 0: the last one at or before
// it. -1 before the first.
inline int beatAt(std::span<const double> beatsMs, double ms)
{
    return static_cast<int>(std::upper_bound(beatsMs.begin(), beatsMs.end(), ms) - beatsMs.begin()) - 1;
}

// Where `ms` lies between two beats: 0 on a beat, up to 1 just before
// the next. -1 before the first beat and from the last one on, where
// there is no next beat to measure against.
inline double beatPhaseAt(std::span<const double> beatsMs, double ms)
{
    const int beat = beatAt(beatsMs, ms);
    if (beat < 0 || beat + 1 >= static_cast<int>(beatsMs.size())) {
        return -1.0;
    }
    const std::size_t i = static_cast<std::size_t>(beat);
    const double length = beatsMs[i + 1] - beatsMs[i];
    return length > 0.0 ? (ms - beatsMs[i]) / length : 0.0;
}

// `levels` (so many a second, from 0 ms) between `fromMs` and `toMs` in
// `columns` columns: each column's loudest entry, and 0 for a column
// that lies outside the track. Every column takes at least one entry,
// so a narrow column still shows what is there. No columns, no
// stretch, or ends that are no numbers: nothing.
inline std::vector<float> loudestPerColumn(std::span<const float> levels, double levelsPerSecond, double fromMs, double toMs,
                                           int columns)
{
    std::vector<float> out;
    if (columns <= 0 || !std::isfinite(fromMs) || !std::isfinite(toMs) || !(toMs > fromMs) || !(levelsPerSecond > 0.0)
        || !std::isfinite(levelsPerSecond)) {
        return out;
    }
    out.reserve(static_cast<std::size_t>(columns));
    const double perMs = levelsPerSecond / 1000.0;
    const double step = (toMs - fromMs) / columns;
    const std::int64_t count = static_cast<std::int64_t>(levels.size());
    for (int column = 0; column < columns; ++column) {
        // Kept within the track (and a step beyond either end) before
        // it becomes a whole number: a moment far outside has none.
        const auto entry = [&](double ms) {
            return static_cast<std::int64_t>(std::clamp(std::floor(ms * perMs), -1.0, static_cast<double>(count) + 1.0));
        };
        const std::int64_t first = entry(fromMs + column * step);
        const std::int64_t last = std::max(first + 1, entry(fromMs + (column + 1) * step));
        float loudest = 0.0f;
        for (std::int64_t i = std::max<std::int64_t>(first, 0); i < std::min(last, count); ++i) {
            loudest = std::max(loudest, levels[static_cast<std::size_t>(i)]);
        }
        out.push_back(loudest);
    }
    return out;
}

}  // namespace seabass::domain
