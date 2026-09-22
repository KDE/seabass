// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// Several hot cues crowded into the first couple of seconds of a track.
// Nobody cues a track three times before the first bar is out; a write
// path that touched one cue list and not the other does.
//
// The case this is for, from a real stick: hot cues at 0.247 s, 1.188 s
// and 1.657 s, alongside two that are plainly real at 1:07.751 and
// 2:15.251. The three early ones lived only in the legacy PCOB list and
// the two real ones only in PCO2 -- the signature of the write path
// fixed in #33. New ones are not being made any more; libraries that
// went through it still carry them.
//
// Why three and not two, and why two seconds. Measured across the
// committed fixture, which carries that very track (anonymized, cue
// positions kept as-is):
//
//   rekordbox, 1161 tracks, 32 with hot cues
//     within 1.0 s: two or more on 0 tracks
//     within 1.5 s: two or more on 1, three or more on 0
//     within 2.0 s: two or more on 2, three or more on 1
//     within 3, 4, 5 and 8 s: unchanged, 2 and 1
//   engine, 1564 tracks, 35 with hot cues
//     within 2.0 s: two or more on 1, three or more on 0
//
// So two-in-a-window is a shape real libraries have (an intro marker and
// the first beat is an ordinary pair) and three is not: exactly one
// track in either catalog has three, and it is the one the issue was
// filed about. Widening the window past two seconds finds nothing more,
// which is the useful part -- the rule is not balanced on the exact
// number.
inline constexpr double ClusterWindowMs = 2000.0;
inline constexpr std::size_t ClusterMinimumCues = 3;

struct ClusteredCueIssue
{
    Track track;
    std::vector<CuePoint> cluster;  // in position order, earliest first
};

// Loops are never part of a cluster. A loop on the first bar is a real
// thing someone set, and it carries an end as well as a start, which a
// stray write never does.
//
// Memory cues are not counted either. A memory cue near the start is
// ordinary (Engine's own analysis puts its main cue a few hundred ms in,
// see junk_cue.hpp), and the crowding that says "something wrote this"
// is a hot-cue pattern: pads, which a person presses one at a time.
class ClusteredCueFinder
{
public:
    static std::vector<ClusteredCueIssue> find(const std::vector<Track> &tracks);
};

// The cues of a cluster that are this check's to offer, which is not the
// whole cluster: one sitting inside the first second is already a stray
// cue by junk_cue.hpp's rule and is already offered there. Listing it
// twice would let a user stage the same removal from two places.
//
// The cluster keeps it either way, because it is evidence: two cues at
// 1.188 s and 1.657 s on their own are a pair, and a pair is something
// real libraries have. It is the third one that makes the shape.
std::vector<CuePoint> removableClusterCues(const ClusteredCueIssue &issue);

}  // namespace seabass::domain
