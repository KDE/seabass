// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "domain/rekordbox_baseline.hpp"

// The rekordbox baseline of Sync after Rekordbox Export on the stick:
// <stick>/Seabass/rekordbox-baseline.tsv.gz (paths::stickRekordboxBaseline,
// docs/sync-after-rekordbox-export-plan.md, "Where the baseline lives").
//
// Format version 1: gzip (so `zcat` reads it on a machine that has never
// heard of Seabass) around tab-separated text, one row per line:
//
//   seabass-rekordbox-baseline<TAB>1<TAB>pdbSequence<TAB>engineUuid<TAB>recordedAtUnix<TAB>writer
//   p<TAB>id<TAB>parentId<TAB>folder(0|1)<TAB>path
//   t<TAB>pathKey<TAB>stickRelativePath<TAB>pdbId<TAB>analysisFile<TAB>rating|-<TAB>ratingOrigin<TAB>comment<TAB>bpm<TAB>durationMs
//   c<TAB>hot|memory<TAB>pad<TAB>positionMs<TAB>loop(0|1)<TAB>loopEndMs<TAB>colour<TAB>origin<TAB>comment
//   m<TAB>playlistId<TAB>position<TAB>pathKey
//   x<TAB>itemKey<TAB>rekordboxStateHash
//   #sha256<TAB>hex-of-everything-above
//
// Sections come in that order: every p, then each t followed by the c
// lines of its cues, then every m, then every x. The playlists, tracks,
// cues and members come back in the order they were written, so a read
// gives the baseline that was written, field for field. An m line's
// position is its index in that playlist's member list (0, 1, 2, ...): a
// track listed twice is two lines. Origins are r (rekordbox), s (Seabass)
// and ? (unknown). Integers are plain decimal, the pdb sequence too;
// positions, loop ends and BPM are the shortest decimal that reads back
// as the same double. Text fields (paths, keys, comments, colours, the
// uuid, the writer, declined hashes) escape tab, newline and backslash as
// \t \n \\, BackupManifest's rule and helpers.
//
// The c line carries the cue's comment, which the plan's grammar left
// out: CuePoint has one, and a round trip has to keep it.
//
// A reader that meets any other version refuses the file: a newer
// Seabass's baseline is not guessed at.
namespace seabass::infrastructure::local
{

inline constexpr int RekordboxBaselineFormatVersion = 1;

// The stick's baseline, verified against its #sha256 line before a field
// is believed.
//
// nullopt with `error` EMPTY: the stick has no baseline (a stick never
// saved since this shipped), which is not an error.
// nullopt with `error` SET, naming the file: it exists and is unreadable,
// truncated, not gzip, fails its hash, has another version, an unknown
// line kind, a malformed field or a section out of order. The caller
// must not treat that as "no baseline": the plan falls back only for a
// stick that has none.
//
// `error` is required; a null pointer throws std::invalid_argument.
std::optional<domain::RekordboxBaseline> readRekordboxBaseline(const std::filesystem::path &stickRoot,
                                                               std::string *error);

// Only the header's pdb sequence, for the badge: inflates until the first
// newline and stops. It does NOT verify the #sha256 line, which needs the
// whole file; readRekordboxBaseline does. A file whose body is damaged
// after an intact header answers here and is refused by the full read.
// Same error rules as readRekordboxBaseline otherwise (missing file:
// nullopt, error empty; malformed or other-version header: error set).
std::optional<std::uint64_t> readRekordboxBaselineSequence(const std::filesystem::path &stickRoot,
                                                           std::string *error);

// Serialises `baseline` in the grammar above, compresses it, calls
// `beforeWrite` with the file's path (UTF-8) before anything on the stick
// changes, so SaveContext can back the old one up, creates the Seabass
// directory if absent, and replaces the file with writeFileDurablyAtomic:
// a reader sees the old file or the new one, never part of either.
//
// False with `error` set, the old file untouched, when the baseline
// cannot be written as it is (a track or member with no pathKey, two
// playlists with one id, a position or BPM that is not finite), the stick
// root is not an existing directory, or the directory or the file cannot
// be written. `beforeWrite` and `error` are required; an empty function or
// a null pointer throws std::invalid_argument.
bool writeRekordboxBaseline(const std::filesystem::path &stickRoot, const domain::RekordboxBaseline &baseline,
                            const std::function<void(const std::string &)> &beforeWrite, std::string *error);

}  // namespace seabass::infrastructure::local
