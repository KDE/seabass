// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include "domain/duplicate_cleanup.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// One duplicate group cleaned up: the doomed copies' cues merged onto the
// survivor, its missing bpm/key/artwork filled in from whichever copy has
// each, the doomed rows removed with their playlists repointed at the
// survivor, and each doomed file recorded for later deletion rather than
// deleted here.
class CleanupGroupChange : public PendingChange
{
public:
    CleanupGroupChange(QString format, QString path, domain::DuplicateCleanupPlan plan, int itemCountHint);

    QString id() const override;
    QString description() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    // The catalogs whose rows this group's doomed copies actually live
    // in. Empty of anything but m_format until rows are collapsed into
    // files -- see application::collapseCatalogRows().
    QStringList doomedRowFormats() const;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_format;
    QString m_path;
    domain::DuplicateCleanupPlan m_plan;
    int m_itemCountHint;
};

}  // namespace seabass::gui
