// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// One player-preference field's new value, staged for the next Save.
class DeviceSettingChange : public PendingChange
{
public:
    DeviceSettingChange(QString pioneerRoot, QString fileName, QString fieldLabel, QString oldValue,
                        QString optionName);

    QString id() const override;
    QString description() const override;
    QString unit() const override;
    QString verb() const override;
    // Settings are not a catalog, so nothing cached needs invalidating.
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_pioneerRoot;
    QString m_fileName;
    QString m_fieldLabel;
    QString m_oldValue;
    QString m_optionName;
};

}  // namespace seabass::gui
