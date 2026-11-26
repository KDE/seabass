// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "infrastructure/local/analysis_ledger_status.hpp"

namespace seabass::infrastructure::local
{

namespace
{

// Both sides name a grid, and not the same one.
bool changedSince(const std::string &recorded, const std::string &now)
{
    return !recorded.empty() && !now.empty() && recorded != now;
}

}  // namespace

bool behindRevisions(const AnalysisLedgerEntry &entry, const CurrentAnalyser &now)
{
    return entry.inputRevision < now.inputRevision || entry.gridRevision < now.gridRevision
           || entry.keyRevision < now.keyRevision || entry.waveformRevision < now.waveformRevision;
}

AnalysisStatus classifyAnalysis(const AnalysisLedgerEntry *entry, const CurrentAnalyser &now, const TrackNow &track)
{
    AnalysisStatus status;
    if (entry == nullptr) {
        status.state = AnalysisState::NotOurs;
        return status;
    }
    status.inputStale = entry->inputRevision < now.inputRevision;
    status.gridStale = entry->gridRevision < now.gridRevision;
    status.keyStale = entry->keyRevision < now.keyRevision;
    status.waveformStale = entry->waveformRevision < now.waveformRevision;
    status.rangeStale = entry->tempoMin != now.tempoMin || entry->tempoMax != now.tempoMax;
    status.rekordboxGridChanged = changedSince(entry->rekordboxGrid, track.rekordboxGrid);
    status.engineGridChanged = changedSince(entry->engineGrid, track.engineGrid);
    status.fileMissing = !track.fileSize.has_value();
    status.mtimeDiffers = track.fileSize && *track.fileSize == entry->fileSize && track.fileMtimeUnix
                          && *track.fileMtimeUnix != entry->fileMtimeUnix;

    if (status.rekordboxGridChanged || status.engineGridChanged) {
        status.state = AnalysisState::GridChangedSince;
    } else if (status.fileMissing || *track.fileSize != entry->fileSize) {
        status.state = AnalysisState::FileChanged;
    } else if (status.inputStale || status.gridStale || status.keyStale || status.waveformStale || status.rangeStale) {
        status.state = AnalysisState::OlderAnalysis;
    } else {
        status.state = AnalysisState::Current;
    }
    return status;
}

}  // namespace seabass::infrastructure::local
