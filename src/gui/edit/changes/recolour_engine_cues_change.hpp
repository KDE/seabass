// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include "domain/hidden_engine_cues.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Writes an Engine track's cue set again, unchanged but for the colour
// of the pads that had none: each takes Engine's default for its pad,
// which is what the cue writer gives a colourless cue since 05d71bbd.
// The player then shows them. See domain::HiddenEngineCues.
class RecolourEngineCuesChange : public PendingChange
{
public:
    // `itemCountHint`: how many of these one save may hold, which decides
    // whether the Engine database is written through a scratch copy.
    RecolourEngineCuesChange(QString enginePath, domain::HiddenEngineCues hidden, int itemCountHint);

    static QString idFor(const std::string &engineSourceId);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_enginePath;
    domain::HiddenEngineCues m_hidden;
    int m_itemCountHint = 1;
};

}  // namespace seabass::gui
