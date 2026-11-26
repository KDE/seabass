// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "infrastructure/local/analysis_ledger_file.hpp"

// What the analysis ledger (analysis_ledger_file.hpp) says about one track
// today: whether Seabass's analysis of it is the one today's analyser
// would make, an older one it could improve, or one that must be left
// alone. Pure: the caller reads the stick, this only compares.
//
// Never decided by analysing the file again and comparing with what is
// stored: the analyser's output differs slightly between compilers and
// processors, so equal input does not give equal bytes everywhere. What
// is compared are the revisions the ledger names and the fingerprints of
// the grids the libraries hold now.
namespace seabass::infrastructure::local
{

// The analyser a new analysis would be made with.
struct CurrentAnalyser
{
    // AnalysisInputRevision.
    int inputRevision = 0;
    // musicanalyze::revisions().
    int gridRevision = 0;
    int keyRevision = 0;
    int waveformRevision = 0;
    // formatTempoBpm() of the ends of the range a new analysis would use.
    std::string tempoMin;
    std::string tempoMax;
};

// The track as the stick has it now.
struct TrackNow
{
    // The audio file's size in bytes; nullopt when it is not on the stick.
    std::optional<std::uint64_t> fileSize;
    // Its modification time, seconds since 1970; nullopt when unknown.
    std::optional<std::int64_t> fileMtimeUnix;
    // The fingerprints of the grids the libraries hold now, as the ledger
    // writes them (rekordbox::pqtzGridFingerprint(), and Engine's
    // BeatGridWriteResult::gridFingerprint); empty where the library is not
    // on the stick, the row has no grid, or it could not be read.
    std::string rekordboxGrid;
    std::string engineGrid;
};

enum class AnalysisState {
    // The analysis is the one today's analyser would make, of this file.
    Current,
    // Made by an older analyser, or with another tempo range: today's
    // could improve it (AnalysisStatus says which parts are behind).
    OlderAnalysis,
    // The file is not the one analysed: another size, or not there.
    FileChanged,
    // A library's grid is no longer the one Seabass left: changed by hand
    // in rekordbox or on a player. Never to be offered for analysis again.
    GridChangedSince,
    // The ledger has no entry: Seabass did not analyse the track, or its
    // record of it is gone.
    NotOurs,
};

struct AnalysisStatus
{
    AnalysisState state = AnalysisState::NotOurs;
    // Which parts are behind today's analyser, whatever the state: the
    // samples the analyser is given (input), its grid, key and waveform
    // revisions, the tempo range (its text differs from today's).
    bool inputStale = false;
    bool gridStale = false;
    bool keyStale = false;
    bool waveformStale = false;
    bool rangeStale = false;
    // Which library's grid has changed since (GridChangedSince).
    bool rekordboxGridChanged = false;
    bool engineGridChanged = false;
    // The file is not on the stick (FileChanged).
    bool fileMissing = false;
    // The modification time differs from the one recorded while the size
    // is the same: a hint, never a state of its own (see classifyAnalysis).
    bool mtimeDiffers = false;
};

// Whether the entry was made by an analyser behind `now` in any revision
// (input, grid, key, waveform). A revision above today's (a newer Seabass
// wrote the entry) is not behind. The tempo range does not count.
bool behindRevisions(const AnalysisLedgerEntry &entry, const CurrentAnalyser &now);

// The track's state, in this order, the first that holds:
//   1. no entry: NotOurs;
//   2. a library's grid fingerprint is recorded, is there now, and
//      differs: GridChangedSince. "-" on either side is no evidence of a
//      change, either way: a library that was not on the stick, a grid the
//      analysis did not give, a row that cannot be read now;
//   3. the file is not on the stick, or its size is not the recorded
//      size: FileChanged;
//   4. a revision behind today's, or the tempo range's text not today's:
//      OlderAnalysis;
//   5. Current.
//
// The size decides whether the file is the one analysed; the
// modification time is a hint only. FAT keeps it to two seconds, and some
// systems shift it by the time zone or summer time, so a different time
// beside the same size is never FileChanged on its own; it sets
// `mtimeDiffers`, for a caller that wants to look closer.
AnalysisStatus classifyAnalysis(const AnalysisLedgerEntry *entry, const CurrentAnalyser &now, const TrackNow &track);

}  // namespace seabass::infrastructure::local
