// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include "domain/library_consistency.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// One Repairable issue: merge whatever cues the broken row(s) have onto the
// survivor, then remove the broken row(s), repointing playlists at the
// survivor rather than dropping them.
class RepairIssueChange : public PendingChange
{
public:
    RepairIssueChange(QString path, domain::LibraryConsistencyIssue issue, int itemCountHint);

    QString id() const override;
    // Repairs and orphan deletions are staged by the same page, so they
    // share one owner (see PendingChange::owner()).
    QString owner() const override;
    QString description() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_path;
    domain::LibraryConsistencyIssue m_issue;
    int m_itemCountHint;
};

}  // namespace seabass::gui
