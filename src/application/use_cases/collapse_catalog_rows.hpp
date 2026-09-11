// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

#include <string>

#include "application/use_cases/find_unreferenced_files.hpp"
#include "domain/track.hpp"

namespace seabass::application
{

// Turns rows into files: one Track per distinct file path, with every
// format's row for it recorded in Track::catalogRows.
//
// DeviceLibrary, OneLibrary and Engine are one library written three
// times, for three hardware products, and are meant to list the same
// tracks. So "three formats listing one file" and "three copies of a
// track" are opposite states -- the first is the library being correct,
// the second is what deduplication exists to remove -- and nothing
// downstream can tell them apart without this step.
//
// DuplicateTrackFinder matches on artist, title and length and has no
// notion of paths, so handed the raw rows of a real stick it groups a
// file with itself: measured on one, 4369 rows point at 1564 files, and
// grouping them uncollapsed put a row for the survivor's own file into
// the removal list of 1156 groups -- 1465 rows that would have been
// deleted for files being kept.
//
// What the collapsed Track carries, and why:
//
// - The FIRST row for a file, in the order given, is the base. Callers
//   pass their catalogs in a fixed order, so the result is stable.
// - Every empty field is then filled from the other rows, first
//   non-empty wins: bitrate, duration, bpm, key, artwork, rating,
//   comment, play count, last played. This is recovery, not guesswork:
//   the formats describe the same library, so a value one of them left
//   out is one another already holds about that same track. It matters
//   concretely -- Engine stores no bitrate at all, so before the fill a
//   file written there scored 0 kbps against its own uncatalogued copy
//   and lost the survivor comparison in every one of a real stick's 435
//   groups.
// - Cues are the union across rows (LocalRestorePlanner::mergeCues),
//   the same rule the cleanup planner uses to merge across copies: a
//   cue that exists in only one catalog is still a cue on that file.
//
// Filling a gap never loses anything, because collapsing removes no row
// from anywhere -- it only stops one file from looking like several.
// Where two formats disagree about the SAME file's rating or comment,
// the first is kept for comparison purposes and the disagreement stays
// exactly where it was, on disk. That disagreement is a real defect --
// the formats are supposed to agree -- but it is synchronization's to
// repair, not deduplication's, and a cleanup must not quietly pick a
// winner while removing something else.
//
// A useful by-product: after this, a file whose catalogRows do not cover
// every format present on the stick is precisely a track one format has
// and another does not. That set difference is the divergence detection
// docs/deduplication-roadmap.md defers to a later Library Health page,
// and it now falls out of a scan rather than needing one of its own.
//
// A track with no resolvable path passes through untouched and alone:
// with nothing to compare, "same file" cannot be established, and
// guessing would merge two real files. Same for a streaming track, whose
// path names a cache on some other machine -- callers filter those out
// before this, and this refuses to fold them together regardless.
// Order matters when an operation is scoped to less than the whole
// library: collapse FIRST, then domain::filterByScope() the files.
//
// Scoping the rows first would hand the collapse a partial set, and a
// file would come out carrying only the rows that happened to be in
// scope -- so removing it would drop those and leave the others still
// pointing at it, which is the exact state the formats are supposed to
// never be in. Collapsing first gives whole files, and TrackScope
// matches a file on any of its rows, so a playlist or a selection that
// names the track in one format still selects the file.
std::vector<domain::Track> collapseCatalogRows(const std::vector<domain::Track> &rows);

// What a cleanup scan should group, given every catalog on the stick.
//
// Two answers, and the difference is a safety rule rather than a
// preference. With every catalog readable, the rows of all of them are
// folded into files, so a plan carries each catalog's row for a file and
// one save can remove them all. With ANY catalog unreadable, this
// returns the caller's single-catalog rows untouched and says so.
//
// A partial collapse is worse than none. A file whose Engine row was
// never seen looks Engine-free, so the plan carries no Engine row, the
// save removes only the rekordbox one, and Engine is left listing a file
// rekordbox no longer does -- the split state deduplication exists to
// prevent, manufactured by deduplication itself. Finding fewer duplicates
// is a poor outcome; misleading the writer about what a file is, is a
// destructive one.
//
// Streaming rows are dropped here rather than at the call site: their
// path names a cache on another machine, so they must never be a
// survivor or a doomed copy, and every caller needed the same filter.
struct CleanupScanRows
{
    std::vector<domain::Track> rows;
    // False when a catalog could not be read and `rows` is therefore the
    // caller's own single-catalog list.
    bool collapsedAcrossCatalogs = false;
};

CleanupScanRows collapseForCleanupScan(const CatalogTracks &catalogs,
                                        const std::vector<std::string> &unreadableCatalogs,
                                        std::vector<domain::Track> singleCatalogRows);

}  // namespace seabass::application
