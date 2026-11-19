// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

// The beat grids Seabass wrote to Engine, on the stick:
// <stick>/Seabass/beat-grids.tsv (paths::stickBeatGridLedger). For each
// track, the fingerprint of the adjusted grid Copy Beat Grids last left
// on its Engine row (BeatGridWriteResult::gridFingerprint). A row whose
// grid still has that fingerprint was changed by Seabass, not on the
// player, and a later copy may write over it.
//
// Format version 1: plain text, so a person can read it, one line each:
//
//   seabass-beat-grids<TAB>1
//   pathKey<TAB>fingerprint
//
// The key is the track's stick-relative path through
// application::normalizedPathKey, never an Engine row id: Engine hands a
// deleted row's id to the next track it adds, and that track's grid
// would then pass for ours. Lines are sorted by key, one per key. The
// fingerprint is 64 lowercase hex digits. A key escapes tab, newline
// and backslash as \t \n \\ (BackupManifest's helpers), so a file name
// holding any of them still makes one line of two fields. Every line
// ends with a newline, the last one too, so a file cut short is seen.
//
// A reader that meets any other version refuses the file.
namespace seabass::infrastructure::local
{

inline constexpr int BeatGridLedgerFormatVersion = 1;

// pathKey to fingerprint.
using BeatGridLedger = std::map<std::string, std::string>;

// The stick's ledger. A missing file is an empty ledger, error empty: a
// stick Copy Beat Grids has not written to. A file that cannot be read,
// or is not this format to the letter (another version, a line without
// its two fields, a bad escape, an empty or repeated key, a fingerprint
// that is not 64 hex digits, lines out of order, no final newline) is
// ALSO an empty ledger, with `error` naming the file and what is wrong:
// nothing in it is believed, so every grid Seabass wrote looks edited on
// the player and is refused, which is the safe way to be wrong. The
// caller reports the error.
//
// `error` is required; a null pointer throws std::invalid_argument.
BeatGridLedger readBeatGridLedger(const std::filesystem::path &stickRoot, std::string *error);

// Replaces the ledger with `ledger` through writeFileDurablyAtomic: a
// reader sees the old file or the new one. Creates the Seabass directory
// if absent. False with `error` set and the old file untouched when an
// entry has an empty key or a fingerprint that is not 64 lowercase hex
// digits, the stick root is not a directory, or the write fails.
// `error` is required; a null pointer throws std::invalid_argument.
bool writeBeatGridLedger(const std::filesystem::path &stickRoot, const BeatGridLedger &ledger, std::string *error);

// The file's text for `ledger`, as writeBeatGridLedger() writes it, for
// a caller that wants to know whether anything would change.
std::string serializeBeatGridLedger(const BeatGridLedger &ledger);

// 64 lowercase hex digits: a SHA-256 as the ledger and the kept analyses
// (kept_engine_analysis_file.hpp) write one.
bool isSha256Hex(std::string_view text);

}  // namespace seabass::infrastructure::local
