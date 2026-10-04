// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Deletes Engine playlist entries that name a track the library no
// longer has (infrastructure::engine::danglingPlaylistEntries): a player
// skips them, and the playlist counts on the stick and on the player then
// disagree. Engine's own trigger relinks the playlist around each one.
class RemoveDanglingPlaylistEntriesChange : public PendingChange
{
public:
    RemoveDanglingPlaylistEntriesChange(QString enginePath, int entries);

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
