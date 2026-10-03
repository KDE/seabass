// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include "domain/track.hpp"
#include "gui/edit/pending_change.hpp"
#include "infrastructure/rekordbox/legacy_memory_list_audit.hpp"

namespace seabass::gui
{

// Puts one rekordbox track's legacy memory cue list back in the shape a
// player reads, every entry kept, and removes the analysis debris a
// hung player left beside it. See legacy_memory_list_audit.hpp (#55).
class RepairLegacyMemoryListChange : public PendingChange
{
public:
    RepairLegacyMemoryListChange(domain::Track track, infrastructure::rekordbox::LegacyMemoryListFinding finding);

    static QString idFor(const std::string &rekordboxSourceId);

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
    domain::Track m_track;
    infrastructure::rekordbox::LegacyMemoryListFinding m_finding;
};

}  // namespace seabass::gui
