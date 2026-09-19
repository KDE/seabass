// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/duplicate_cue_consolidation.hpp"

#include <cmath>
#include <map>
#include <numeric>
#include <optional>

#include "domain/matching_policy.hpp"
#include "domain/track_matching.hpp"

namespace seabass::domain
{

namespace
{

// Whether two files hold the same music, when their stored lengths are
// close but not close enough to settle it on their own.
//
// Only the length of what is NOT silent is compared. Two copies of one
// recording routinely differ by several seconds of encoder padding,
// run-out or a trimmed re-export, and that difference is silence on one
// side; the music between the silences is the same length on both. A
// difference that survives the trim is a different edit.
//
// nullopt from either side is "no opinion" and never a match: no
// decoder in this build, a file the backend refused, a file no longer
// on the stick. The caller is about to offer to delete something.
bool audioContentAgrees(const Track &a, const Track &b, AudioContentProbe &probe)
{
    if (a.filePath.empty() || b.filePath.empty()) {
        return false;
    }
    const std::optional<AudioContentSpan> spanA = probe.measure(a.filePath);
    if (!spanA) {
        return false;
    }
    const std::optional<AudioContentSpan> spanB = probe.measure(b.filePath);
    if (!spanB) {
        return false;
    }
    // A file that decoded to nothing but silence tells us nothing about
    // which recording it is, and comparing two of them would call every
    // silent file a copy of every other.
    if (spanA->contentSeconds() <= 0.0 || spanB->contentSeconds() <= 0.0) {
        return false;
    }
    return std::abs(spanA->contentSeconds() - spanB->contentSeconds()) <= MatchingPolicy::exactMatchSeconds();
}

size_t findRoot(std::vector<size_t> &parent, size_t x)
{
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

void unite(std::vector<size_t> &parent, size_t a, size_t b)
{
    parent[findRoot(parent, a)] = findRoot(parent, b);
}

}  // namespace

std::vector<DuplicateGroup> DuplicateTrackFinder::find(const std::vector<Track> &tracks,
                                                       AudioContentProbe *probe)
{
    // Tracks are candidates for the same underlying song when they share
    // a title+artist. Filename is deliberately NOT a matching criterion:
    // it carries an export-assigned track-number prefix and a copy
    // suffix (`034_Artist-Title.mp3`, `...-1.mp3`) that differ between
    // copies of the same song, and conversely two genuinely different
    // songs can be exported under the same truncated name. Measured on a
    // real 1469-track Engine stick, adding filename matching found
    // exactly zero groups that title+artist did not already find, so it
    // only ever contributed risk.
    std::vector<size_t> parent(tracks.size());
    std::iota(parent.begin(), parent.end(), size_t{0});

    std::map<std::string, std::vector<size_t>> byTitleArtist;
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (auto key = titleArtistKey(tracks[i])) {
            byTitleArtist[*key].push_back(i);
        }
    }
    for (const auto &[key, indices] : byTitleArtist) {
        for (size_t k = 1; k < indices.size(); ++k) {
            unite(parent, indices[0], indices[k]);
        }
    }

    std::map<size_t, std::vector<size_t>> byRoot;
    for (size_t i = 0; i < tracks.size(); ++i) {
        byRoot[findRoot(parent, i)].push_back(i);
    }

    std::vector<DuplicateGroup> groups;
    for (auto &[root, indices] : byRoot) {
        if (indices.size() < 2) {
            continue;
        }

        // Within a candidate set, cluster by duration tolerance -- two
        // unrelated tracks that happen to share a filename or title+artist
        // (e.g. a cover version) shouldn't be treated as duplicates.
        //
        // Two passes, and the split is the whole point. The first uses
        // stored lengths alone and is exactly what this did before the
        // audio comparison existed. The second may only ADD to what the
        // first found: it attaches tracks the first pass left on their
        // own, and never merges or breaks up a cluster the first pass
        // built.
        //
        // A single greedy pass that consulted the probe inline could
        // LOSE a group. With the exact window at 2 s and the wider one
        // at 10 s, take A=300 s, B=306 s, C=308 s under one title. Alone,
        // A matches neither, then B and C are 2 s apart and {B, C} is
        // offered. With the probe consulted inline, the seed A claims B
        // (the audio agrees), B is marked used, C is left by itself, and
        // {B, C} is never formed -- so switching the setting ON would
        // have made a duplicate pair disappear from Clean Up.
        //
        // durationSeconds == 0 means "unreadable/unknown" here, same
        // fallback convention as the rest of Track's fields (e.g.
        // bitrate), not a real zero-length track.
        //
        // An unknown duration is treated as "cannot confirm these are
        // the same recording", NOT as "close enough". This reverses an
        // earlier rule that grouped a pair whenever either side's
        // duration was missing: that rule was introduced to stop a real
        // duplicate cluster being split when two of three copies had a
        // failed duration reading, but it is the wrong trade for a
        // *destructive* caller. Measured on a real Engine stick where
        // 77.6% of rows had no duration (Engine leaves `length` NULL
        // until it has analyzed a track), the permissive rule put a
        // radio edit and an extended mix of the same title+artist in one
        // group 40 times, and the Clean Up planner had them checked for
        // deletion in 39 of those -- roughly an hour of unique audio,
        // silently. Title+artist alone cannot tell a 2:47 edit from a
        // 6:31 extended mix; only the length can, so without a length
        // there is no match.
        constexpr size_t NoCluster = static_cast<size_t>(-1);
        std::vector<std::vector<size_t>> clusters;  // positions into `indices`
        std::vector<size_t> clusterOf(indices.size(), NoCluster);

        auto lengthOf = [&](size_t pos) { return tracks[indices[pos]].durationSeconds; };
        auto bothLengthsKnown = [&](size_t a, size_t b) {
            return lengthOf(a) > 0.0 && lengthOf(b) > 0.0;
        };

        // Pass one: stored lengths, exact window.
        for (size_t i = 0; i < indices.size(); ++i) {
            if (clusterOf[i] != NoCluster) {
                continue;
            }
            const size_t here = clusters.size();
            clusters.push_back({i});
            clusterOf[i] = here;
            for (size_t j = i + 1; j < indices.size(); ++j) {
                if (clusterOf[j] != NoCluster || !bothLengthsKnown(i, j)) {
                    continue;
                }
                if (std::abs(lengthOf(i) - lengthOf(j)) <= MatchingPolicy::exactMatchSeconds()) {
                    clusters[here].push_back(j);
                    clusterOf[j] = here;
                }
            }
        }

        // Pass two: past the exact window, and inside the wider one the
        // user set, the lengths stop being decisive and the audio is
        // asked instead. Only tracks still on their own are offered, and
        // only to clusters that already exist, so nothing pass one built
        // can be taken apart.
        //
        // Decoding is orders of magnitude more expensive than comparing
        // two numbers, so it is paid for the handful of tracks that are
        // genuinely in doubt and never for the rest.
        if (probe != nullptr) {
            for (size_t c = 0; c < clusters.size(); ++c) {
                if (clusters[c].empty()) {
                    continue;  // absorbed into an earlier cluster
                }
                const size_t seed = clusters[c].front();
                for (size_t other = 0; other < clusters.size(); ++other) {
                    if (other == c || clusters[other].size() != 1) {
                        continue;  // only a track still on its own
                    }
                    const size_t lone = clusters[other].front();
                    if (!bothLengthsKnown(seed, lone)) {
                        continue;
                    }
                    const double gap = std::abs(lengthOf(seed) - lengthOf(lone));
                    if (gap <= MatchingPolicy::exactMatchSeconds()
                        || gap > MatchingPolicy::compareAudioSeconds()) {
                        // Inside the exact window pass one already had
                        // its say; beyond the wider one these are
                        // different recordings and nothing is decoded.
                        continue;
                    }
                    if (audioContentAgrees(tracks[indices[seed]], tracks[indices[lone]], *probe)) {
                        clusters[c].push_back(lone);
                        clusters[other].clear();
                        clusterOf[lone] = c;
                    }
                }
            }
        }

        for (const auto &cluster : clusters) {
            if (cluster.size() < 2) {
                continue;
            }
            DuplicateGroup group;
            for (size_t pos : cluster) {
                group.tracks.push_back(tracks[indices[pos]]);
            }
            groups.push_back(std::move(group));
        }
    }

    return groups;
}

ConsolidationPlan DuplicateCueConsolidator::plan(const DuplicateGroup &group)
{
    ConsolidationPlan result;
    result.group = group;

    std::vector<const Track *> withCues;
    for (const auto &track : group.tracks) {
        if (!track.cues.empty()) {
            withCues.push_back(&track);
        }
    }

    if (withCues.empty()) {
        result.kind = ConsolidationPlan::Kind::NoCues;
        return result;
    }

    if (withCues.size() == 1) {
        result.kind = ConsolidationPlan::Kind::Unambiguous;
        result.source = *withCues.front();
        for (const auto &track : group.tracks) {
            if (track.sourceId != result.source->sourceId) {
                result.targets.push_back(track);
            }
        }
        return result;
    }

    for (size_t i = 1; i < withCues.size(); ++i) {
        if (!cueSetsEqual(withCues[0]->cues, withCues[i]->cues)) {
            result.kind = ConsolidationPlan::Kind::Conflict;
            return result;
        }
    }
    result.kind = ConsolidationPlan::Kind::AlreadyConsistent;
    return result;
}

}  // namespace seabass::domain
