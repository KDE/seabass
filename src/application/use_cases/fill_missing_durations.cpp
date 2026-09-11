// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/fill_missing_durations.hpp"

namespace seabass::application
{

FillMissingDurationsResult fillMissingDurations(std::vector<domain::Track> &tracks, TrackDurationProbe &probe,
                                                 DurationCachePort *cache)
{
    FillMissingDurationsResult result;

    // Several catalog rows routinely point at the same file (the very
    // duplicates this feeds), and probing opens and decodes the file --
    // so remember what this run already resolved and never pay twice.
    std::map<std::string, std::optional<double>> resolvedThisRun;

    for (auto &track : tracks) {
        if (track.durationSeconds > 0.0) {
            result.alreadyKnown++;
            continue;
        }
        if (track.filePath.empty()) {
            result.unreadable++;
            continue;
        }

        auto seen = resolvedThisRun.find(track.filePath);
        if (seen != resolvedThisRun.end()) {
            if (seen->second) {
                track.durationSeconds = *seen->second;
            } else {
                result.unreadable++;
            }
            continue;
        }

        if (cache) {
            if (auto cached = cache->lookup(track.filePath)) {
                track.durationSeconds = *cached;
                resolvedThisRun[track.filePath] = cached;
                result.fromCache++;
                continue;
            }
        }

        auto probed = probe.durationSeconds(track.filePath);
        resolvedThisRun[track.filePath] = probed;
        if (!probed || *probed <= 0.0) {
            result.unreadable++;
            continue;
        }
        track.durationSeconds = *probed;
        result.probed++;
        if (cache) {
            cache->store(track.filePath, *probed);
        }
    }

    return result;
}

}  // namespace seabass::application
