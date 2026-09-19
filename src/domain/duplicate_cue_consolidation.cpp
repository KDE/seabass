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
        std::vector<bool> used(indices.size(), false);
        for (size_t i = 0; i < indices.size(); ++i) {
            if (used[i]) {
                continue;
            }
            DuplicateGroup group;
            group.tracks.push_back(tracks[indices[i]]);
            used[i] = true;
            for (size_t j = i + 1; j < indices.size(); ++j) {
                if (used[j]) {
                    continue;
                }
                // durationSeconds == 0 means "unreadable/unknown" here,
                // same fallback convention as the rest of Track's fields
                // (e.g. bitrate), not a real zero-length track.
                //
                // An unknown duration is treated as "cannot confirm these
                // are the same recording", NOT as "close enough". This
                // reverses an earlier rule that grouped a pair whenever
                // either side's duration was missing: that rule was
                // introduced to stop a real duplicate cluster being split
                // when two of three copies had a failed duration reading,
                // but it is the wrong trade for a *destructive* caller.
                // Measured on a real Engine stick where 77.6% of rows had
                // no duration (Engine leaves `length` NULL until it has
                // analyzed a track), the permissive rule put a radio edit
                // and an extended mix of the same title+artist in one
                // group 40 times, and the Clean Up planner had them
                // checked for deletion in 39 of those -- roughly an hour
                // of unique audio, silently. Title+artist alone cannot
                // tell a 2:47 edit from a 6:31 extended mix; only the
                // length can, so without a length there is no match.
                double durationA = tracks[indices[i]].durationSeconds;
                double durationB = tracks[indices[j]].durationSeconds;
                bool bothDurationsKnown = durationA > 0.0 && durationB > 0.0;
                if (!bothDurationsKnown) {
                    continue;
                }
                const double gap = std::abs(durationA - durationB);
                bool sameRecording = gap <= MatchingPolicy::exactMatchSeconds();
                // Past the exact-match window, and inside the wider one
                // the user set, the lengths stop being decisive and the
                // audio is asked instead. Only here: decoding is orders
                // of magnitude more expensive than comparing two numbers,
                // so it is paid for the handful of pairs that are
                // genuinely in doubt and never for the rest.
                if (!sameRecording && probe != nullptr && gap <= MatchingPolicy::compareAudioSeconds()) {
                    sameRecording = audioContentAgrees(tracks[indices[i]], tracks[indices[j]], *probe);
                }
                if (sameRecording) {
                    group.tracks.push_back(tracks[indices[j]]);
                    used[j] = true;
                }
            }
            if (group.tracks.size() >= 2) {
                groups.push_back(std::move(group));
            }
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
