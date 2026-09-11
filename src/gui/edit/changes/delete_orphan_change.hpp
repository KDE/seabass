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

// One Missing issue's orphaned OneLibrary row(s) deleted outright. Only
// OneLibrary rows are ever deleted this way: the other catalogs get a
// repair (see RepairIssueChange) because their rows can be repointed at a
// survivor instead of dropped.
class DeleteOrphanChange : public PendingChange
{
public:
    DeleteOrphanChange(QString path, domain::LibraryConsistencyIssue issue);

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
};

}  // namespace seabass::gui
