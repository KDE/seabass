// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Repoints Engine playlist entries the player cannot see to the track's
// origin uuid (infrastructure::engine::repointUnresolvedPlaylistEntries):
// an entry the player cannot resolve through (originDatabaseUuid,
// originTrackId) is left out of the playlist on the player, as the 120
// Seabass wrote with the library's own uuid were on WHALESHARK3. A copy of
// a track the list holds visibly already goes; an entry no uuid would
// make resolve is left alone and the log says how many.
class RepointPlaylistEntriesChange : public PendingChange
{
public:
    RepointPlaylistEntriesChange(QString enginePath, int entries);

    static QString idFor();

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString unit() const override;
    QString verb() const override;
    int unitsWritten() const override { return m_entries; }
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_enginePath;
    int m_entries = 0;
};

}  // namespace seabass::gui
