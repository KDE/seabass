// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <memory>
#include <string>

#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Sync after Rekordbox Export: Engine's playlist tree brought in line with
// rekordbox's, one playlist per change, each through the save's Engine
// write session, m.db declared, Engine 1.x refused, a playlist named by
// its full path ("Folder/List", as the Engine reader spells it). Folders
// are playlists in Engine (a playlist with children), so the same changes
// make and rename both. Unit "playlists".
//
// Staged in the plan's order (creates parents first, then the track adds,
// renames parents first, memberships, ..., deletes deepest first), so a
// change finds what the ones before it made.

// Creates the playlist at `path` with every missing playlist above it
// (createEnginePlaylist), each last among its siblings. A path Engine
// already has exactly once is a skip (made since the scan, by the player
// or an earlier change); one it spells more than once fails the change.
// Afterwards the path must name exactly one playlist.
class CreateEnginePlaylistChange : public PendingChange
{
public:
    CreateEnginePlaylistChange(QString enginePath, std::string path, int itemCountHint);

    static QString idFor(const std::string &path);

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
    std::string m_path;
    int m_itemCountHint = 1;
};

// Renames the playlist at `from` to `to` (renameEnginePlaylist): a new
// title in place, or a move under the existing playlist `to`'s parent path
// names, last there. It keeps its id, its entries in their order and its
// children. Already done (no playlist at `from`, exactly one at `to`) is a
// skip; every other refusal of renameEnginePlaylist fails the change.
// Afterwards `from` must name none and `to` exactly one.
class RenameEnginePlaylistChange : public PendingChange
{
public:
    RenameEnginePlaylistChange(QString enginePath, std::string from, std::string to, int itemCountHint);

    static QString idFor(const std::string &from);

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
    std::string m_from;
    std::string m_to;
    int m_itemCountHint = 1;
};

// Deletes the playlist at `path` from Engine only, with every playlist
// below it; the tracks stay. Browse's DeletePlaylistChange with no
// PIONEER root and the Engine library alone, wrapped in OwnedChange for
// this page. Its id is "rekordbox-export-sync:addcue:delete-playlist:"
// plus the path; it refuses a path Engine spells twice and skips one it
// no longer has.
std::unique_ptr<PendingChange> deleteEnginePlaylistChange(QString enginePath, std::string path);

}  // namespace seabass::gui
