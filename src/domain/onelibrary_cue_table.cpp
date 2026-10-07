// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/onelibrary_cue_table.hpp"

#include <algorithm>

#include "domain/cue_tolerance.hpp"

namespace seabass::domain
{

std::vector<CuePoint> cuesNotInFile(const std::vector<CuePoint> &table, const std::vector<CuePoint> &file,
                                    double toleranceMs)
{
    std::vector<CuePoint> missing;
    for (const auto &t : table) {
        const bool found = std::any_of(file.begin(), file.end(), [&](const CuePoint &f) {
            return f.kind == t.kind && (t.kind == CuePoint::Kind::Memory || f.hotCueNumber == t.hotCueNumber)
                && sameCuePlace(t, f, toleranceMs);
        });
        if (!found) {
            missing.push_back(t);
        }
    }
    return missing;
}

CueTableAudit auditCueTables(const std::vector<Track> &rows,
                             const std::function<std::vector<CuePoint>(const Track &)> &tableOf)
{
    CueTableAudit audit;
    for (const auto &row : rows) {
        ++audit.rowsRead;
        auto table = tableOf(row);
        if (table.empty()) {
            ++audit.emptyTable;
            continue;
        }
        auto notInFile = cuesNotInFile(table, row.cues, cueToleranceMsFor(row.bpm, row.bpm));
        if (notInFile.empty()) {
            ++audit.withinFile;
            continue;
        }
        audit.excess.push_back({row, std::move(table), std::move(notInFile)});
    }
    return audit;
}

}  // namespace seabass::domain
