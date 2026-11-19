// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/beat_grid.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

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

namespace
{
// How far a beat may sit off the straight line of its span and still be
// that span's. rekordbox stores beat times in whole milliseconds, so an
// evenly spaced grid comes back from the file up to half a millisecond off
// its line, and the line through two such beats is itself up to half a
// millisecond off the true one: a millisecond takes both, and is less
// than anyone can hear.
constexpr double kBeatOffLineMs = 1.0;
// How many beats a span is looked ahead past the last one that could
// close it, see markersFromBeats().
constexpr std::size_t kFurthestLookAhead = 512;
}  // namespace

MarkersFromBeats markersFromBeats(const std::vector<Beat> &source, double durationMs)
{
    // Beats too close to the one before are dropped, the earlier kept:
    // beatsFromMarkers() refuses a span of beats that short, and one such
    // pair would cost the whole grid.
    std::vector<Beat> beats;
    for (const Beat &beat : beatsInOrder(source)) {
        if (beats.empty() || beat.timeMs - beats.back().timeMs >= kShortestBeatMs) {
            beats.push_back(beat);
        }
    }
    // Two beats are the fewest that make a tempo. More than any track has
    // is a damaged grid, and would not fit Engine's indices either.
    if (beats.size() < 2 || beats.size() > kMostBeats) {
        return {};
    }
    const std::size_t count = beats.size();

    // Index 0 is a downbeat and every fourth after it. The first beat that
    // says where in the bar it is decides where the count starts.
    const auto known = [](int beatInBar) { return beatInBar >= 1 && beatInBar <= 4; };
    std::size_t reference = count;
    for (std::size_t i = 0; i < count; ++i) {
        if (known(beats[i].beatInBar)) {
            reference = i;
            break;
        }
    }
    int firstIndex = 0;
    if (reference < count) {
        const int behind = static_cast<int>(reference % 4);
        firstIndex = ((beats[reference].beatInBar - 1 - behind) % 4 + 4) % 4;
    }

    // A break is a beat whose place in the bar does not follow from the
    // last beat that said one. Only the break itself is counted, not every
    // beat after it that Engine now counts differently from the source.
    MarkersFromBeats result;
    std::size_t lastKnown = reference;
    for (std::size_t i = reference + 1; i < count; ++i) {
        if (!known(beats[i].beatInBar)) {
            continue;
        }
        const std::size_t sinceKnown = static_cast<std::size_t>(beats[lastKnown].beatInBar - 1) + (i - lastKnown);
        if (beats[i].beatInBar != static_cast<int>(sinceKnown % 4) + 1) {
            ++result.barBreaks;
        }
        lastKnown = i;
    }

    // Spans, each as long as it can be, from the start. While a span is
    // extended from beat `from`, [shortest, longest] holds every beat
    // length that keeps each beat passed so far within kBeatOffLineMs of
    // the line; a beat can close the span when the length that reaches it
    // exactly lies in there. Once no length is left, the span closes at the
    // last beat that could, and the next one starts there.
    std::vector<std::size_t> pinned = {0};
    std::size_t from = 0;
    while (from + 1 < count) {
        double shortest = -std::numeric_limits<double>::infinity();
        double longest = std::numeric_limits<double>::infinity();
        std::size_t closesAt = from + 1;
        for (std::size_t j = from + 1; j < count; ++j) {
            const double steps = static_cast<double>(j - from);
            const double sinceFrom = beats[j].timeMs - beats[from].timeMs;
            const double exact = sinceFrom / steps;
            if (exact >= shortest && exact <= longest) {
                closesAt = j;
            }
            shortest = std::max(shortest, (sinceFrom - kBeatOffLineMs) / steps);
            longest = std::min(longest, (sinceFrom + kBeatOffLineMs) / steps);
            // On a real grid the span closes at nearly every beat it
            // passes. One beat a millisecond early right after `from`,
            // and the rest a little late, keeps a length possible to the
            // end of the track while no beat closes the span: the whole
            // track looked through for a span of one beat. No input was
            // found that does this over and over, and none was proven
            // not to exist; with the limit the cost is bounded whatever
            // the file says.
            if (shortest > longest || j - closesAt >= kFurthestLookAhead) {
                break;
            }
        }
        pinned.push_back(closesAt);
        from = closesAt;
    }
    for (const std::size_t i : pinned) {
        result.markers.push_back({firstIndex + static_cast<int>(i), beats[i].timeMs});
    }

    // The whole track between the first marker and the last: the ends move
    // out by whole beats of their own span, so they stay on its line. The
    // lengths are taken before either end moves, as with one span both do.
    const auto beatLength = [&](std::size_t a, std::size_t b) {
        return (beats[pinned[b]].timeMs - beats[pinned[a]].timeMs) / static_cast<double>(pinned[b] - pinned[a]);
    };
    const double firstBeatMs = beatLength(0, 1);
    const double lastBeatMs = beatLength(pinned.size() - 2, pinned.size() - 1);
    // An end further out than any track has beats is left where it is,
    // rather than pinned to an index that does not fit.
    BeatGridMarker &first = result.markers.front();
    if (first.timeMs > 0.0) {
        double back = std::ceil(first.timeMs / firstBeatMs);
        // Rounding can leave it a hair after the start; one more beat then.
        if (first.timeMs - back * firstBeatMs > 0.0) {
            back += 1.0;
        }
        if (back <= static_cast<double>(kMostBeats)) {
            first.index -= static_cast<int>(back);
            first.timeMs -= back * firstBeatMs;
        }
    }
    BeatGridMarker &last = result.markers.back();
    if (std::isfinite(durationMs) && durationMs > 0.0 && last.timeMs < durationMs) {
        double on = std::ceil((durationMs - last.timeMs) / lastBeatMs);
        if (last.timeMs + on * lastBeatMs < durationMs) {
            on += 1.0;
        }
        if (on <= static_cast<double>(kMostBeats)) {
            last.index += static_cast<int>(on);
            last.timeMs += on * lastBeatMs;
        }
    }
    return result;
}

std::vector<BeatGridMarker> engineNumbered(std::vector<BeatGridMarker> markers)
{
    if (markers.size() < 2) {
        return markers;
    }
    for (const BeatGridMarker &marker : markers) {
        if (!std::isfinite(marker.timeMs)) {
            return markers;
        }
    }
    const BeatGridMarker &first = markers[0];
    const BeatGridMarker &second = markers[1];
    if (second.index <= first.index) {
        return markers;
    }
    // In 64 bits: the indices are whatever the caller had, and moving
    // them must not wrap.
    const long long firstIndex = first.index;
    const double beatMs = (second.timeMs - first.timeMs) / static_cast<double>(static_cast<long long>(second.index) - firstIndex);
    if (!(beatMs > 0.0) || !std::isfinite(beatMs)) {
        return markers;
    }
    const auto timeOf = [&](long long index) { return first.timeMs + static_cast<double>(index - firstIndex) * beatMs; };

    // The first index on the line at or after the start, then the first
    // downbeat from there. Worked out, then nudged: the division can land
    // a hair either side of a beat that sits exactly at 0.
    const double atStart = static_cast<double>(firstIndex) - first.timeMs / beatMs;
    if (!std::isfinite(atStart) || std::abs(atStart) > static_cast<double>(std::numeric_limits<int>::max() / 2)) {
        return markers;
    }
    const auto downbeatFrom = [](long long index) { return index + ((4 - index % 4) % 4); };
    long long downbeat = downbeatFrom(static_cast<long long>(std::ceil(atStart)));
    while (timeOf(downbeat - 4) >= 0.0) {
        downbeat -= 4;
    }
    while (timeOf(downbeat) < 0.0) {
        downbeat += 4;
    }
    // The new first marker stays on the first span's line only while it
    // is before the second marker: past it, the line is another span's.
    if (downbeat - 4 >= static_cast<long long>(second.index)) {
        return markers;
    }
    for (const BeatGridMarker &marker : markers) {
        const long long moved = static_cast<long long>(marker.index) - downbeat;
        if (moved < std::numeric_limits<int>::min() || moved > std::numeric_limits<int>::max()) {
            return markers;
        }
    }
    const double firstTimeMs = timeOf(downbeat - 4);
    markers[0] = {static_cast<int>(downbeat - 4), firstTimeMs};
    for (BeatGridMarker &marker : markers) {
        marker.index = static_cast<int>(static_cast<long long>(marker.index) - downbeat);
    }
    return markers;
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
