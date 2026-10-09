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

// Sync after Rekordbox Export: one Engine row rekordbox no longer has,
// taken out of the Engine library with every row that names it
// (infrastructure::engine::removeEngineTrackRows): its playlist entries in
// every playlist, Engine's trigger relinking each list around them, its
// prepare-list entries and its performance data, then the Track row. The
// audio file stays (it is rekordbox's), and so does a shared AlbumArt row.
//
// Unlike DeleteTracksChange, this DOES remove a track that is in
// playlists: the sync's membership removes and its track removals both
// come through here, and a track rekordbox dropped is dropped from Engine's
// playlists with it. DeleteTracksChange (Library Health) leaves a listed
// track alone; that rule is its own and not this change's.
//
// The row is named by its Engine id and confirmed by its file: at apply
// time the id has to be one of the rows engineTrackIdsForFile finds for
// the row's filePath, in the save's write root. No row for the file any
// more is a skip (gone since the scan). A file whose rows do not include
// the id fails the change: the library changed under the page, and
// removing whatever now names the file is not what was asked.
//
// Writes the save's one Engine write session, backs up m.db, refuses
// Engine 1.x. After the removal no row of any table names the id, or the
// change fails and the save loop puts m.db back.
class RemoveEngineTrackChange : public PendingChange
{
public:
    // `engineRow`: Engine's row as read (TrackToRemove::engine): sourceId,
    // filePath, title and artist.
    RemoveEngineTrackChange(QString enginePath, domain::Track engineRow, int itemCountHint);

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
    domain::Track m_row;
    int m_itemCountHint = 1;
};

}  // namespace seabass::gui
