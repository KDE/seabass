// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "application/use_cases/fill_missing_durations.hpp"

namespace seabass::application
{

namespace
{

// What this run already worked out about one file. The origin is kept
// alongside the length because the rows after the first are filled from
// it too, and a filled row has to be counted under the thing it came
// from -- the counters are what the run reports it did, and a row
// counted nowhere is a row the report says nothing about.
struct Resolved
{
    enum class Origin { Cache, Probe };
    std::optional<double> seconds;  // nothing = no length is available for this file
    Origin origin = Origin::Probe;
};

}  // namespace

FillMissingDurationsResult fillMissingDurations(std::vector<domain::Track> &tracks, TrackDurationProbe &probe,
                                                 DurationCachePort *cache)
{
    FillMissingDurationsResult result;

    // Several catalog rows routinely point at the same file (the very
    // duplicates this feeds), and probing opens and decodes the file --
    // so remember what this run already resolved and never pay twice.
    std::map<std::string, Resolved> resolvedThisRun;

    auto count = [&result](Resolved::Origin origin) {
        if (origin == Resolved::Origin::Cache) {
            result.fromCache++;
        } else {
            result.probed++;
        }
    };

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
            if (seen->second.seconds) {
                track.durationSeconds = *seen->second.seconds;
                count(seen->second.origin);
            } else {
                result.unreadable++;
            }
            continue;
        }

        if (cache) {
            if (auto cached = cache->lookup(track.filePath)) {
                track.durationSeconds = *cached;
                resolvedThisRun[track.filePath] = {cached, Resolved::Origin::Cache};
                result.fromCache++;
                continue;
            }
        }

        auto probed = probe.durationSeconds(track.filePath);
        if (!probed || *probed <= 0.0) {
            // Remembered as "no length", not as the zero it answered:
            // handing that zero to this file's other rows would leave
            // them looking filled while carrying nothing, and would
            // count them as read.
            resolvedThisRun[track.filePath] = {std::nullopt, Resolved::Origin::Probe};
            result.unreadable++;
            continue;
        }
        resolvedThisRun[track.filePath] = {probed, Resolved::Origin::Probe};
        track.durationSeconds = *probed;
        result.probed++;
        if (cache) {
            cache->store(track.filePath, *probed);
        }
    }

    return result;
}

}  // namespace seabass::application
