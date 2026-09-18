// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstdint>
#include <vector>

#include <QString>
#include <QStringList>

#include "gui/edit/pending_change.hpp"
#include "infrastructure/engine/engine_sample_rates.hpp"

namespace seabass::gui
{

// Writes the sample rate each track's own file reports into the Engine
// rows that do not carry one.
//
// One change per track, like every other fix in Library Health. It was
// one change for the whole set at first, which made the Save button and
// the summary count it as a single item while forty-three tracks were
// being written: the same work, counted differently from the repairs and
// the cover art beside it, so nothing on the page added up.
//
// declaresDatabase: true for the first of a batch only, the same
// reasoning as RepairArtworkChange::filesToBackup() -- one checkpoint
// copy of m.db per save rather than one per track.
class FillSampleRateChange : public PendingChange
{
public:
    FillSampleRateChange(QString enginePath, infrastructure::engine::SampleRateEntry entry, int itemCountHint,
                         bool declaresDatabase = true);

    static QString idFor(std::int64_t trackId);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_enginePath;
    infrastructure::engine::SampleRateEntry m_entry;
    int m_itemCountHint = 1;
    bool m_declaresDatabase = true;
};

}  // namespace seabass::gui
