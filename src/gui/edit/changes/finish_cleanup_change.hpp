// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include "domain/cleanup_leftovers.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Finishes one removal a Clean Up made in export.pdb and never made in
// OneLibrary (see domain::CleanupLeftover): the OneLibrary rows at the
// removed file go, and their playlist entries move onto the copy Clean Up
// kept -- the same call Clean Up makes today, for the same pair.
//
// One file per change, because that is what the summary counts and what
// the page lists. The database is backed up once for the batch: each
// removal is its own transaction, rolled back on failure inside the
// writer, so a whole-file checkpoint per change would copy exportLibrary.db
// three hundred times to protect nothing (the lesson RepairArtworkChange
// wrote down for m.db).
class FinishCleanupChange : public PendingChange
{
public:
    // `pioneerRoot` is the stick's PIONEER folder; `declaresDatabase` is
    // true for the first change of a batch only.
    FinishCleanupChange(QString pioneerRoot, domain::CleanupLeftover leftover, bool declaresDatabase);

    static QString idFor(const std::string &leftoverFilePath);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_pioneerRoot;
    domain::CleanupLeftover m_leftover;
    bool m_declaresDatabase = false;
};

}  // namespace seabass::gui
