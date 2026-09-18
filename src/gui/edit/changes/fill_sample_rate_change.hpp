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
// One change for the whole set, unlike the artwork repair's one per
// track: there is nothing to decide per row here and no per-row UI to
// keep in step -- every entry writes one number that the file already
// answered for during the scan, and a save summary reading "1 change"
// against "sample rates filled in for 43 tracks" says what happened
// better than 43 identical lines would.
class FillSampleRateChange : public PendingChange
{
public:
    FillSampleRateChange(QString enginePath, std::vector<infrastructure::engine::SampleRateEntry> entries);

    static QString idFor();

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
    std::vector<infrastructure::engine::SampleRateEntry> m_entries;
};

}  // namespace seabass::gui
