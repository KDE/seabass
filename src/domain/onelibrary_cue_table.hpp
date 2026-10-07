// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "domain/track.hpp"

namespace seabass::domain
{

// #57: builds before afa3dbdb could write a OneLibrary row's cue table on
// its own, with another copy's cues. The players read the analysis file
// (#59), so a table holding a cue the file does not is what those builds
// left. An empty table is not damage (rekordbox and the OMNIS-DUO leave
// the table alone), and neither is a table the file holds more than (the
// file adds rekordbox's memory cue at 0:00).

// One row of OneLibrary's cue table. kind 0 is a memory cue and 1 to 8 a
// hot cue slot; any other kind is one Seabass does not understand, and
// such a row is never matched, listed or removed.
struct CueTableEntry
{
    int64_t cueId = 0;
    int64_t kind = 0;
    CuePoint cue;  // meaningful only when understood()

    bool understood() const { return kind >= 0 && kind <= 8; }
};

// The entries of `table` Seabass understands that `file` does not hold: a
// hot cue on the same pad at the same place, a memory cue at the same
// place (sameCuePlace), less than `toleranceMs` apart. Empty when the
// table is within the file.
std::vector<CueTableEntry> entriesNotInFile(const std::vector<CueTableEntry> &table, const std::vector<CuePoint> &file,
                                            double toleranceMs);

// Whether OneLibraryReader read this row's cues from its analysis file:
// the row names one, and it was there and decoded. A row it could not
// read carries no cues, which says nothing about its table. Cautious: a
// file whose cue lists could not be compared counts as not read.
bool analysisFileRead(const Track &row);

// A row whose cue table holds cues its analysis file does not.
struct CueTableExcess
{
    Track row;  // the OneLibrary row, its cues the analysis file's
    std::vector<CueTableEntry> table;
    std::vector<CueTableEntry> notInFile;  // what the repair removes
};

// Every row read falls in exactly one count.
struct CueTableAudit
{
    int rowsRead = 0;
    int emptyTable = 0;
    int withinFile = 0;
    // Cues in the table and no analysis file read: left alone, since the
    // table may be the only copy of the track's cues.
    int noReadableFile = 0;
    // Within the file but for cue kinds Seabass does not understand.
    int notUnderstood = 0;
    std::vector<CueTableExcess> excess;  // in the order the rows came
};

// Each OneLibrary row's table (`tableOf`) against its cues, which are the
// analysis file's (OneLibraryReader), at half a beat of the row's tempo.
CueTableAudit auditCueTables(const std::vector<Track> &rows,
                             const std::function<std::vector<CueTableEntry>(const Track &)> &tableOf);

// "pad A at 0:00.247, memory cue at 0:04.399, pad B loop at 1:07.751 to
// 1:08.251", by position: how Library Health and stick_damage_audit name
// a list of cues.
std::string describeCuePlaces(std::vector<CuePoint> cues);

// The understood entries' cues.
std::vector<CuePoint> cuesOf(const std::vector<CueTableEntry> &entries);

}  // namespace seabass::domain
