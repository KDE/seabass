// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <vector>

#include "domain/track.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// The one copy of a duplicate group that has the cues, and every other
// copy they are to be written onto.
struct DuplicatesCopyOp
{
    domain::Track source;
    std::vector<domain::Track> targets;
};

// One duplicate group: copy the chosen copy's cues onto every other copy.
class CopyCuesChange : public PendingChange
{
public:
    CopyCuesChange(QString format, QString path, QString groupKey, DuplicatesCopyOp op);

    QString id() const override;
    QString description() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_format;
    QString m_path;
    QString m_groupKey;
    DuplicatesCopyOp m_op;
};

}  // namespace seabass::gui
