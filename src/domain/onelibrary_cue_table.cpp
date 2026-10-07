// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "domain/onelibrary_cue_table.hpp"

#include <algorithm>

#include "domain/cue_tolerance.hpp"
#include "domain/sync_planning.hpp"

namespace seabass::domain
{

std::vector<CueTableEntry> entriesNotInFile(const std::vector<CueTableEntry> &table, const std::vector<CuePoint> &file,
                                            double toleranceMs)
{
    std::vector<CueTableEntry> missing;
    for (const auto &t : table) {
        if (!t.understood()) {
            continue;
        }
        const bool found = std::any_of(file.begin(), file.end(), [&](const CuePoint &f) {
            return f.kind == t.cue.kind && (t.cue.kind == CuePoint::Kind::Memory || f.hotCueNumber == t.cue.hotCueNumber)
                && sameCuePlace(t.cue, f, toleranceMs);
        });
        if (!found) {
            missing.push_back(t);
        }
    }
    return missing;
}

bool cueListsRead(Track::CueListsCheck check)
{
    return check == Track::CueListsCheck::Examined || check == Track::CueListsCheck::Disagree;
}

bool analysisFileRead(const Track &row)
{
    return !row.analysisFile.empty() && cueListsRead(row.cueLists);
}

CueTableCheck checkCueTable(const std::vector<CueTableEntry> &table,
                            const std::optional<std::vector<CuePoint>> &fileCues, double toleranceMs)
{
    CueTableCheck check;
    if (table.empty()) {
        return check;
    }
    if (!fileCues) {
        check.verdict = CueTableVerdict::NoFileRead;
        return check;
    }
    if (!std::all_of(table.begin(), table.end(), [](const CueTableEntry &e) { return e.understood(); })) {
        check.verdict = CueTableVerdict::NotUnderstood;
        return check;
    }
    check.notInFile = entriesNotInFile(table, *fileCues, toleranceMs);
    check.verdict = check.notInFile.empty() ? CueTableVerdict::WithinFile : CueTableVerdict::Excess;
    return check;
}

CueTableAudit auditCueTables(const std::vector<Track> &rows,
                             const std::function<std::vector<CueTableEntry>(const Track &)> &tableOf)
{
    CueTableAudit audit;
    for (const auto &row : rows) {
        ++audit.rowsRead;
        auto table = tableOf(row);
        const auto fileCues = analysisFileRead(row) ? std::optional<std::vector<CuePoint>>(row.cues) : std::nullopt;
        auto check = checkCueTable(table, fileCues, cueToleranceMsFor(row.bpm, row.bpm));
        switch (check.verdict) {
        case CueTableVerdict::Empty:
            ++audit.emptyTable;
            break;
        case CueTableVerdict::NoFileRead:
            ++audit.noReadableFile;
            break;
        case CueTableVerdict::NotUnderstood:
            ++audit.notUnderstood;
            break;
        case CueTableVerdict::WithinFile:
            ++audit.withinFile;
            break;
        case CueTableVerdict::Excess:
            audit.excess.push_back({row, std::move(table), std::move(check.notInFile)});
            break;
        }
    }
    return audit;
}

std::string describeCuePlaces(std::vector<CuePoint> cues)
{
    std::stable_sort(cues.begin(), cues.end(), [](const CuePoint &a, const CuePoint &b) { return a.positionMs < b.positionMs; });
    std::string out;
    for (const auto &c : cues) {
        out += out.empty() ? "" : ", ";
        out += c.kind == CuePoint::Kind::Hot ? std::string("pad ") + static_cast<char>('A' + c.hotCueNumber - 1)
                                             : std::string("memory cue");
        out += (c.isLoop ? " loop at " : " at ") + formatCuePosition(c.positionMs);
        if (c.isLoop) {
            out += " to " + formatCuePosition(c.loopEndMs);
        }
    }
    return out;
}

std::vector<CuePoint> cuesOf(const std::vector<CueTableEntry> &entries)
{
    std::vector<CuePoint> cues;
    for (const auto &e : entries) {
        if (e.understood()) {
            cues.push_back(e.cue);
        }
    }
    return cues;
}

}  // namespace seabass::domain
