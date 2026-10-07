// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <functional>
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

// The cues `table` holds that `file` does not: a hot cue on the same pad
// at the same place, a memory cue at the same place (sameCuePlace), less
// than `toleranceMs` apart. Empty when the table is within the file.
std::vector<CuePoint> cuesNotInFile(const std::vector<CuePoint> &table, const std::vector<CuePoint> &file,
                                    double toleranceMs);

// A row whose cue table holds cues its analysis file does not.
struct CueTableExcess
{
    Track row;  // the OneLibrary row, its cues the analysis file's
    std::vector<CuePoint> table;
    std::vector<CuePoint> notInFile;  // what the table holds beyond the file
};

struct CueTableAudit
{
    int rowsRead = 0;
    int emptyTable = 0;
    int withinFile = 0;
    std::vector<CueTableExcess> excess;  // in the order the rows came
};

// Each OneLibrary row's table (`tableOf`) against its cues, which are the
// analysis file's (OneLibraryReader), at half a beat of the row's tempo.
CueTableAudit auditCueTables(const std::vector<Track> &rows,
                             const std::function<std::vector<CuePoint>(const Track &)> &tableOf);

}  // namespace seabass::domain
