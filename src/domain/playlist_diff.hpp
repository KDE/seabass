// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <string>
#include <vector>

namespace seabass::domain
{

// A git-style diff of two playlists, read as sets first and sequences
// second: what one list has that the other lacks is the finding, and a
// track that is in both but somewhere else is reported as moved, never as
// missing. The lists are sequences of track identities (one catalog's
// sourceId, or whatever key the caller chose); a track listed twice is
// two entries.
//
// Rows are produced in the order a side-by-side view shows them: the
// longest common subsequence of the two lists pins the rows where both
// sides agree, and everything between two such rows is laid out as the
// A-only entries beside the B-only entries, top-aligned.
// One side of a row. None: this side is blank on this row.
enum class DiffEntryKind {
    None,
    Same,   // the same track at an agreed point in both orders
    Only,   // this list has the track, the other does not
    Moved,  // both lists have the track, at different points (see partner)
};

struct DiffEntry
{
    DiffEntryKind kind = DiffEntryKind::None;
    int index = -1;    // index into this side's list, or -1 for None
    int partner = -1;  // Moved: the first index of the same track in the other list
};

struct DiffRow
{
    DiffEntry a;
    DiffEntry b;
    bool same() const { return a.kind == DiffEntryKind::Same; }
};

struct PlaylistDiff
{
    std::vector<DiffRow> rows;
    int shared = 0;  // entries of A whose track is also in B
    int onlyA = 0;   // entries of A whose track is not in B
    int onlyB = 0;   // entries of B whose track is not in A
    int moved = 0;   // shared entries the common subsequence could not pin: a different order

    bool sameTracks() const { return onlyA == 0 && onlyB == 0; }
    bool identical() const { return sameTracks() && moved == 0; }
};

PlaylistDiff diffPlaylists(const std::vector<std::string> &a, const std::vector<std::string> &b);

// How B stands to A, as a set, for picking what to compare against: the
// "Whaleshark Spacy Techno" that is the first thirty of "Spacy Techno",
// the "Spacy / Acid Techno" that is "Spacy Techno" plus ten.
enum class PlaylistRelation {
    Disjoint,   // nothing in common
    Identical,  // the same tracks (the order may still differ)
    Superset,   // B has every track of A, and more
    Subset,     // B is part of A
    Overlap,    // some shared, each has its own
};

struct PlaylistOverlap
{
    PlaylistRelation relation = PlaylistRelation::Disjoint;
    int shared = 0;  // distinct tracks in both
    int onlyA = 0;   // distinct tracks only in A
    int onlyB = 0;   // distinct tracks only in B
    // shared / (distinct tracks in either); 1 for identical sets, 0 for disjoint.
    double jaccard = 0.0;
};

PlaylistOverlap overlapOf(const std::vector<std::string> &a, const std::vector<std::string> &b);

}  // namespace seabass::domain
