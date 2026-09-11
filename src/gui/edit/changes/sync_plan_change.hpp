// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include "domain/sync_planning.hpp"
#include "domain/track.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// One sync plan: the source track's cues copied onto the matched track in
// the other catalog. The plan's direction decides which of the matched
// pair is the source and which the target.
class SyncPlanChange : public PendingChange
{
public:
    SyncPlanChange(QString rekordboxPath, QString enginePath, domain::SyncPlan plan, int itemCountHint);

    const domain::Track &target() const;
    const domain::Track &source() const;

    QString id() const override;
    QString description() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_rekordboxPath;
    QString m_enginePath;
    domain::SyncPlan m_plan;
    int m_itemCountHint;
};

}  // namespace seabass::gui
