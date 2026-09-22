// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// fillMissingDurations() decides what length a track has, and length is
// the only field that separates a radio edit from an extended mix of the
// same artist and title. Clean Up groups duplicates on it and offers the
// group's losers for deletion, so a wrong length here is a deletion
// proposal about the wrong file.
//
// The counters are the other half. They are what the UI reports the run
// did, and this project has a standing habit of counters that stay
// confident while the work underneath them changes: every case below
// asserts that the four of them still add up to the number of tracks
// that went in. A track whose length was filled and counted nowhere is
// the failure this pins down.

#include <cassert>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "application/use_cases/fill_missing_durations.hpp"

using seabass::application::DurationCachePort;
using seabass::application::fillMissingDurations;
using seabass::application::FillMissingDurationsResult;
using seabass::application::TrackDurationProbe;
using seabass::domain::Track;

namespace
{

// Counts its calls, because "never re-probed" is a claim about how often
// this was reached, and only the probe can answer it. Probing opens and
// decodes an audio file off a USB stick; a second one is a second read.
class CountingProbe : public TrackDurationProbe
{
public:
    std::map<std::string, std::optional<double>> answers;
    std::map<std::string, int> calls;

    std::optional<double> durationSeconds(const std::string &path) override
    {
        calls[path]++;
        const auto found = answers.find(path);
        return found == answers.end() ? std::nullopt : found->second;
    }

    int totalCalls() const
    {
        int total = 0;
        for (const auto &[path, count] : calls) {
            total += count;
        }
        return total;
    }
};

class MemoryCache : public DurationCachePort
{
public:
    std::map<std::string, double> entries;
    mutable int lookups = 0;
    int stores = 0;

    std::optional<double> lookup(const std::string &path) const override
    {
        lookups++;
        const auto found = entries.find(path);
        return found == entries.end() ? std::optional<double>{} : found->second;
    }

    void store(const std::string &path, double seconds) override
    {
        stores++;
        entries[path] = seconds;
    }
};

Track trackAt(const std::string &path, double seconds = 0.0)
{
    Track track;
    track.filePath = path;
    track.durationSeconds = seconds;
    return track;
}

// Every track that went in came out counted as exactly one of the four
// things that can happen to it. Asserted in every case rather than once,
// because the hole this closes only opens on particular shapes of input.
void everyTrackAccountedFor(const FillMissingDurationsResult &result, size_t tracks)
{
    assert(result.alreadyKnown + result.fromCache + result.probed + result.unreadable == tracks);
}

}  // namespace

int main()
{
    // A catalog that already knows the length is never opened. Both
    // because reading it again costs a file decode, and because a value
    // derived here that disagreed with the catalog's own would be drift
    // nobody asked for.
    {
        CountingProbe probe;
        probe.answers["/stick/a.mp3"] = 999.0;
        MemoryCache cache;
        cache.entries["/stick/a.mp3"] = 888.0;
        std::vector<Track> tracks{trackAt("/stick/a.mp3", 210.5)};

        const auto result = fillMissingDurations(tracks, probe, &cache);

        assert(tracks[0].durationSeconds == 210.5);
        assert(probe.totalCalls() == 0);
        assert(cache.lookups == 0);
        assert(result.alreadyKnown == 1);
        everyTrackAccountedFor(result, tracks.size());
    }

    // The cache answers first, and a cache hit is not a probe.
    {
        CountingProbe probe;
        probe.answers["/stick/a.mp3"] = 999.0;
        MemoryCache cache;
        cache.entries["/stick/a.mp3"] = 180.0;
        std::vector<Track> tracks{trackAt("/stick/a.mp3")};

        const auto result = fillMissingDurations(tracks, probe, &cache);

        assert(tracks[0].durationSeconds == 180.0);
        assert(probe.totalCalls() == 0);
        assert(result.fromCache == 1);
        assert(cache.stores == 0);  // it was already there
        everyTrackAccountedFor(result, tracks.size());
    }

    // A miss is probed, filled and written back, so the next run over
    // the same stick is a cache hit.
    {
        CountingProbe probe;
        probe.answers["/stick/a.mp3"] = 240.0;
        MemoryCache cache;
        std::vector<Track> tracks{trackAt("/stick/a.mp3")};

        const auto result = fillMissingDurations(tracks, probe, &cache);

        assert(tracks[0].durationSeconds == 240.0);
        assert(probe.totalCalls() == 1);
        assert(result.probed == 1);
        assert(cache.stores == 1);
        assert(cache.entries["/stick/a.mp3"] == 240.0);
        everyTrackAccountedFor(result, tracks.size());
    }

    // No cache at all is a supported configuration, not a crash.
    {
        CountingProbe probe;
        probe.answers["/stick/a.mp3"] = 240.0;
        std::vector<Track> tracks{trackAt("/stick/a.mp3")};

        const auto result = fillMissingDurations(tracks, probe, nullptr);

        assert(tracks[0].durationSeconds == 240.0);
        assert(result.probed == 1);
        everyTrackAccountedFor(result, tracks.size());
    }

    // A stale row pointing at a deleted file, and a row with no path at
    // all. Both are ordinary things to meet on a real stick, and neither
    // may leave a fabricated length behind: a made-up length is a
    // duplicate group, and a duplicate group is a deletion offer.
    {
        CountingProbe probe;  // answers nothing
        MemoryCache cache;
        std::vector<Track> tracks{trackAt("/stick/gone.mp3"), trackAt("")};

        const auto result = fillMissingDurations(tracks, probe, &cache);

        assert(tracks[0].durationSeconds == 0.0);
        assert(tracks[1].durationSeconds == 0.0);
        assert(result.unreadable == 2);
        assert(cache.stores == 0);  // "I could not read it" is not a length
        assert(probe.totalCalls() == 1);  // the empty path is not worth a probe
        everyTrackAccountedFor(result, tracks.size());
    }

    // A probe that answers zero or a negative number has not read a
    // length either, whatever it returned.
    {
        CountingProbe probe;
        probe.answers["/stick/zero.mp3"] = 0.0;
        probe.answers["/stick/negative.mp3"] = -3.0;
        MemoryCache cache;
        std::vector<Track> tracks{trackAt("/stick/zero.mp3"), trackAt("/stick/negative.mp3")};

        const auto result = fillMissingDurations(tracks, probe, &cache);

        assert(result.unreadable == 2);
        assert(result.probed == 0);
        assert(cache.stores == 0);
        assert(tracks[0].durationSeconds == 0.0);
        assert(tracks[1].durationSeconds == 0.0);
        everyTrackAccountedFor(result, tracks.size());
    }

    // The case the memo exists for: several catalog rows pointing at one
    // file, which is the very shape Clean Up is run to find. One probe,
    // every row filled, and every row counted.
    {
        CountingProbe probe;
        probe.answers["/stick/shared.mp3"] = 300.0;
        MemoryCache cache;
        std::vector<Track> tracks{trackAt("/stick/shared.mp3"), trackAt("/stick/shared.mp3"),
                                  trackAt("/stick/shared.mp3")};

        const auto result = fillMissingDurations(tracks, probe, &cache);

        assert(probe.totalCalls() == 1);
        assert(cache.stores == 1);
        for (const Track &track : tracks) {
            assert(track.durationSeconds == 300.0);
        }
        // All three came off the same probe, and all three must be
        // somewhere in the counters. Counting only the row that paid for
        // the probe reports one filled track out of three.
        assert(result.probed == 3);
        everyTrackAccountedFor(result, tracks.size());
    }

    // The same, served from the cache rather than the probe.
    {
        CountingProbe probe;
        MemoryCache cache;
        cache.entries["/stick/shared.mp3"] = 150.0;
        std::vector<Track> tracks{trackAt("/stick/shared.mp3"), trackAt("/stick/shared.mp3")};

        const auto result = fillMissingDurations(tracks, probe, &cache);

        assert(probe.totalCalls() == 0);
        assert(cache.lookups == 1);  // the second row is answered by the memo
        assert(tracks[0].durationSeconds == 150.0);
        assert(tracks[1].durationSeconds == 150.0);
        assert(result.fromCache == 2);
        everyTrackAccountedFor(result, tracks.size());
    }

    // An unreadable file met twice is unreadable twice, and a file that
    // probed as zero must not have that zero handed to its other rows as
    // though it were an answer.
    {
        CountingProbe probe;
        probe.answers["/stick/zero.mp3"] = 0.0;
        MemoryCache cache;
        std::vector<Track> tracks{trackAt("/stick/gone.mp3"), trackAt("/stick/gone.mp3"),
                                  trackAt("/stick/zero.mp3"), trackAt("/stick/zero.mp3")};

        const auto result = fillMissingDurations(tracks, probe, &cache);

        assert(probe.calls["/stick/gone.mp3"] == 1);
        assert(probe.calls["/stick/zero.mp3"] == 1);
        assert(result.unreadable == 4);
        assert(result.probed == 0);
        everyTrackAccountedFor(result, tracks.size());
    }

    // A whole small stick at once, which is how this is actually called:
    // a mix of all four outcomes in one pass.
    {
        CountingProbe probe;
        probe.answers["/stick/c.mp3"] = 420.0;
        MemoryCache cache;
        cache.entries["/stick/b.mp3"] = 200.0;
        std::vector<Track> tracks{trackAt("/stick/a.mp3", 100.0), trackAt("/stick/b.mp3"),
                                  trackAt("/stick/c.mp3"),       trackAt("/stick/c.mp3"),
                                  trackAt("/stick/d.mp3"),       trackAt("")};

        const auto result = fillMissingDurations(tracks, probe, &cache);

        assert(result.alreadyKnown == 1);
        assert(result.fromCache == 1);
        assert(result.probed == 2);
        assert(result.unreadable == 2);
        everyTrackAccountedFor(result, tracks.size());
    }

    std::cout << "fill_missing_durations_test passed\n";
    return 0;
}
