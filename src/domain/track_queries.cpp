// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/track_queries.hpp"

#include <algorithm>

namespace seabass::domain
{

namespace
{

bool hasEngagementSignal(const Track &track)
{
    return track.playCount.has_value() || track.lastPlayedAt.has_value();
}

// True if `a` should be considered more worth setting cues on than `b`.
bool morePriority(const Track &a, const Track &b)
{
    bool aHas = hasEngagementSignal(a);
    bool bHas = hasEngagementSignal(b);
    if (aHas != bHas) {
        return aHas;
    }
    if (!aHas) {
        return false;  // neither has a signal -- preserve relative order
    }
    if (a.playCount && b.playCount) {
        return *a.playCount > *b.playCount;
    }
    if (a.lastPlayedAt && b.lastPlayedAt) {
        return *a.lastPlayedAt > *b.lastPlayedAt;
    }
    // Mixed signal types (rare -- would mean tracks from both formats in
    // one list): arbitrarily but consistently prefer play-count signals.
    return a.playCount.has_value();
}

}  // namespace

std::vector<Track> TrackPrioritizer::tracksNeedingCues(const std::vector<Track> &tracks, size_t limit)
{
    std::vector<Track> result;
    for (const auto &track : tracks) {
        if (track.cues.empty()) {
            result.push_back(track);
        }
    }

    std::stable_sort(result.begin(), result.end(), morePriority);

    if (limit > 0 && result.size() > limit) {
        result.resize(limit);
    }
    return result;
}

}  // namespace seabass::domain
