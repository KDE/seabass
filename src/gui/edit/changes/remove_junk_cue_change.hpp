// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include "domain/track.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// One track's 0:00 memory cue removed: the full cue list rewritten without
// it, the same "pass the complete replacement set" contract every other
// cue write here follows.
class RemoveJunkCueChange : public PendingChange
{
public:
    RemoveJunkCueChange(QString path, domain::Track track);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_path;
    domain::Track m_track;
};

}  // namespace seabass::gui
