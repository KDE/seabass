// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

#include "application/ports/cancellation_token.hpp"
#include "application/ports/metadata_cache_port.hpp"
#include "application/ports/track_metadata_probe.hpp"
#include "application/use_cases/find_unreferenced_files.hpp"
#include "domain/track.hpp"

namespace seabass::application
{

// Turns the files findUnreferencedFiles() found into domain::Tracks, so
// they can go through DuplicateTrackFinder::find() alongside the
// catalogued ones and be reviewed as part of the same duplicate cleanup.
// No new matching logic anywhere: a stray file becomes an ordinary
// Track, and every rule that then applies to it is one the planner
// already had, plus the two that read Track::isUnreferenced and
// Track::durationIsEstimated.
//
// What a stray file can and cannot fill in is decided by where the
// values come from. Title, artist, duration and bitrate are read off the
// file itself. Rating, comment, play count, cues and key are not: they
// live in a catalog row, and this file has none. They stay empty rather
// than being guessed at, which is also why a stray file can never be the
// reason a group is flagged as having unpreservable data at risk.
//
// Files the probe cannot read at all are dropped: with no duration there
// is nothing to group on, and a file this code cannot understand is the
// last thing that should reach a deletion proposal. They stay on the
// stick, unmentioned by anything -- the same outcome as never having
// been walked.
//
// `cache` may be null, which simply means every file is read fresh.
// Reading tags off a real 2100-file stick costs 22-66 s cold over USB
// against a stat per file once cached, so a caller with a stick in hand
// should pass one (infrastructure::local::MetadataCache keeps its store
// on the stick, where the answers belong).
std::vector<domain::Track> unreferencedFilesAsTracks(const std::vector<AudioFileOnDisk> &files,
                                                       TrackMetadataProbe &probe, MetadataCachePort *cache,
                                                       const CancellationToken &cancel);

}  // namespace seabass::application
