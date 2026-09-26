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
                                                 DurationCachePort *cache, CachedDurations trust,
                                                 CancellationToken cancel)
{
    FillMissingDurationsResult result;

    // Several catalog rows routinely point at the same file (the very
    // duplicates this feeds), and probing opens and decodes the file --
    // so remember what this run already resolved and never pay twice.
    std::map<std::string, Resolved> resolvedThisRun;

    const bool unverified = trust == CachedDurations::Unverified;
    auto count = [&result, unverified](Resolved::Origin origin) {
        if (origin == Resolved::Origin::Cache) {
            result.fromCache++;
            if (unverified) {
                result.fromCacheUnverified++;
            }
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
                track.durationIsProbed = true;
                count(seen->second.origin);
            } else {
                result.unreadable++;
            }
            continue;
        }

        // Once per file, before it is looked at or opened: a pulled
        // stick stops the fill here rather than after every other file.
        cancel.throwIfCancelled();
        if (cache) {
            const auto cached =
                unverified ? cache->lookupUnverified(track.filePath) : cache->lookup(track.filePath);
            if (cached) {
                track.durationSeconds = *cached;
                track.durationIsProbed = true;
                resolvedThisRun[track.filePath] = {cached, Resolved::Origin::Cache};
                count(Resolved::Origin::Cache);
                if (unverified) {
                    result.unverifiedPaths.push_back(track.filePath);
                }
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
        track.durationIsProbed = true;
        result.probed++;
        if (cache) {
            cache->store(track.filePath, *probed);
        }
    }

    return result;
}

VerifyCachedDurationsResult verifyCachedDurations(std::vector<domain::Track> &tracks,
                                                   const std::vector<std::string> &paths, TrackDurationProbe &probe,
                                                   DurationCachePort &cache, CancellationToken cancel)
{
    VerifyCachedDurationsResult result;
    for (const std::string &path : paths) {
        cancel.throwIfCancelled();
        const std::optional<double> taken = cache.lookupUnverified(path);
        if (!taken) {
            continue;  // nothing was taken from the cache for this file
        }
        if (cache.lookup(path)) {
            result.confirmed++;
            continue;
        }
        // Changed or gone since it was probed: what a Verified fill would
        // have done for it, a probe, and the cache corrected.
        const std::optional<double> probed = probe.durationSeconds(path);
        double now = 0.0;
        if (probed && *probed > 0.0) {
            now = *probed;
            cache.store(path, *probed);
            result.reprobed++;
        } else {
            result.unreadable++;
        }
        for (auto &track : tracks) {
            if (track.filePath == path && track.durationSeconds == *taken) {
                track.durationSeconds = now;
                result.tracksUpdated++;
            }
        }
    }
    return result;
}

}  // namespace seabass::application
