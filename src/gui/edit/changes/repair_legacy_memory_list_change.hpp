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

// Puts one analysis file's legacy memory cue list back in the shape a
// player reads, every entry kept (#55); when its legacy and modern cue
// lists disagree, rewrites both from the one `keep` names (#60); and
// removes the analysis debris a hung player left beside it. See
// legacy_memory_list_audit.hpp.
class RepairLegacyMemoryListChange : public PendingChange
{
public:
    RepairLegacyMemoryListChange(domain::Track track, infrastructure::rekordbox::LegacyMemoryListFinding finding,
                                 infrastructure::rekordbox::KeepCueList keep
                                 = infrastructure::rekordbox::KeepCueList::Player);

    // By the analysis file, not the row: a DeviceLibrary row and a
    // OneLibrary row can name the same file, and it is repaired once.
    static QString idFor(const std::string &analyzePath);

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
    // The lists are rewritten, and exportLibrary.db may list the file.
    bool refreshesOneLibraryTable() const;

    domain::Track m_track;
    infrastructure::rekordbox::LegacyMemoryListFinding m_finding;
    infrastructure::rekordbox::KeepCueList m_keep;
};

}  // namespace seabass::gui
