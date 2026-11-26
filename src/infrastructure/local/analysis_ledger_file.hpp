// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>

// The analyses Seabass wrote to the stick, on the stick:
// <stick>/Seabass/analysis.tsv.gz (paths::stickAnalysisLedger). For each
// track whose analysis by Seabass's own analyser (lib/musicanalyze) a save
// wrote into the stick's libraries, by the track import or by Analyse
// again: which revisions of the analyser made it, from which file, with
// which tempo range, and the beat grid each library was left with. From it
// a later Seabass can tell a track an older analyser analysed, which a
// newer one could improve, from a track whose grid was changed since by
// hand or on a player, which is never to be analysed over
// (analysis_ledger_status.hpp).
//
// Format version 1: gzip (so `zcat` reads it) around tab-separated text:
//
//   seabass-analysis<TAB>1
//   t<TAB>pathKey<TAB>fileSize<TAB>fileMtimeUnix<TAB>inputRev<TAB>gridRev<TAB>keyRev<TAB>waveformRev<TAB>libraryVersion<TAB>tempoMin<TAB>tempoMax<TAB>rekordboxGrid|-<TAB>engineGrid|-<TAB>key|-<TAB>keyWritten(0|1)<TAB>analysedAtUnix
//   #sha256<TAB>hex-of-everything-above
//
// - pathKey: the audio file's stick-relative path through
//   application::normalizedPathKey, never a row id (a library hands a
//   deleted row's id to the next track it adds). One line per key, the
//   lines sorted by key.
// - fileSize, fileMtimeUnix: the audio file on the stick when it was
//   analysed, in bytes and in seconds since 1970 (UTC).
// - inputRev: AnalysisInputRevision (infrastructure/audio/
//   qt_multimedia_pcm_source.hpp), what samples the analyser was given.
//   gridRev, keyRev, waveformRev: musicanalyze::Revisions of the result.
//   libraryVersion: musicanalyze::version(), for a person reading the
//   file; nothing compares it.
// - tempoMin, tempoMax: the tempo range the analysis ran with, in BPM
//   with exactly two decimals (formatTempoBpm()). Compared as the text,
//   never as numbers read back.
// - rekordboxGrid: the fingerprint of the beat grid Seabass left in the
//   rekordbox analysis file (rekordbox::pqtzGridFingerprint()).
//   engineGrid: the fingerprint of the grid it left on Engine's row
//   (BeatGridWriteResult::gridFingerprint, the beat grid ledger's).
//   Each 64 lowercase hex digits, or "-": no such library on the stick,
//   the analysis gave no grid, or the library kept a grid of its own.
//   "-" says nothing about later changes, either way.
// - key: the key the analyser heard, as rekordbox spells it, "-" for
//   none; keyWritten: 1 when it went into a library row (only a row that
//   named no key takes it), 0 otherwise. Never 1 beside "-".
// - analysedAtUnix: when the save that wrote the analysis recorded it.
//
// Text fields (the key, libraryVersion, the pathKey) escape tab, newline
// and backslash as \t \n \\, BackupManifest's rule and helpers. Every
// line ends with a newline, the last one too.
//
// A reader that meets any other version refuses the file.
namespace seabass::infrastructure::local
{

inline constexpr int AnalysisLedgerFormatVersion = 1;

struct AnalysisLedgerEntry
{
    std::string pathKey;
    std::uint64_t fileSize = 0;
    std::int64_t fileMtimeUnix = 0;
    int inputRevision = 0;
    int gridRevision = 0;
    int keyRevision = 0;
    int waveformRevision = 0;
    std::string libraryVersion;
    // As written: formatTempoBpm() of the range's ends.
    std::string tempoMin;
    std::string tempoMax;
    // 64 lowercase hex digits, or empty for "-".
    std::string rekordboxGrid;
    std::string engineGrid;
    // Empty for "-".
    std::string key;
    bool keyWritten = false;
    std::int64_t analysedAtUnix = 0;

    bool operator==(const AnalysisLedgerEntry &other) const = default;
};

// pathKey to its entry; each entry's pathKey is its key here.
using AnalysisLedger = std::map<std::string, AnalysisLedgerEntry>;

// A tempo as the ledger writes it: exactly two decimals, a point, no
// sign and no exponent ("70.00", "175.00"). Empty for a tempo that is not
// a finite number from 0 to 1000.
std::string formatTempoBpm(double bpm);

// The stick's ledger. A missing file is an empty ledger, `error` empty:
// a stick Seabass has written no analysis to. A file that exists and
// cannot be read, or is not this format to the letter (not gzip, cut
// short, a #sha256 line missing or not matching, another version, an
// unknown line kind, a line with the wrong number of fields, a field that
// does not parse, keys out of order or repeated, no final newline) is
// ALSO an empty ledger, with `error` naming the file and what is wrong:
// nothing in it is believed. The caller reports it; a later write
// replaces the file.
//
// `error` is required; a null pointer throws std::invalid_argument.
AnalysisLedger readAnalysisLedger(const std::filesystem::path &stickRoot, std::string *error);

// The file's text for `ledger` before compression, as
// writeAnalysisLedger() writes it, #sha256 line included; nullopt with
// `problem` set when an entry cannot be written as it is (below).
std::optional<std::string> serializeAnalysisLedger(const AnalysisLedger &ledger, std::string *problem);

// Serialises `ledger`, compresses it, calls `beforeWrite` with the file's
// path (UTF-8) before anything on the stick changes, so a save can back
// the old file up, creates the Seabass directory if absent, and replaces
// the file with writeFileDurablyAtomic: a reader sees the old file or
// the new one, never part of either.
//
// False with `error` set, the old file untouched, when an entry cannot be
// written as it is (an empty key, a key that is not its entry's pathKey,
// a tempo or a fingerprint not in the form above, a negative revision,
// keyWritten without a key), the stick root is not an existing directory,
// or the directory or the file cannot be written. `beforeWrite` and
// `error` are required; an empty function or a null pointer throws
// std::invalid_argument.
bool writeAnalysisLedger(const std::filesystem::path &stickRoot, const AnalysisLedger &ledger,
                         const std::function<void(const std::string &)> &beforeWrite, std::string *error);

}  // namespace seabass::infrastructure::local
