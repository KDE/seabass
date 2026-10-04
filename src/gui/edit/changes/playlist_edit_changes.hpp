// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// The libraries on the stick that hold a playlist at `playlist` (its full
// path, "Folder/List"), read now: "rekordbox", "onelibrary", "engine".
// What a playlist edit from Browse will write, and what it backs up.
QStringList librariesWithPlaylist(const QString &pioneerRoot, const QString &enginePath, const std::string &playlist);
// The libraries where more than one playlist spells that path (two of one
// name in a folder, or a name holding a "/"): editing by path would have
// to guess there, so Browse refuses, and the changes fail rather than
// guess if it happens by Save.
QStringList librariesWithSeveralPlaylists(const QString &pioneerRoot, const QString &enginePath, const std::string &playlist);

// Deletes a playlist (or a folder, with everything in it) from every
// library on the stick that has it, each through its own writer, the way
// rekordbox does it (#62). Tracks stay. Staged from Browse, so its owner
// is Browse's editor ("addcue").
class DeletePlaylistChange : public PendingChange
{
public:
    DeletePlaylistChange(QString pioneerRoot, QString enginePath, std::string playlist, QStringList libraries);

    static QString idFor(const std::string &playlist);

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
    QString m_pioneerRoot;
    QString m_enginePath;
    std::string m_playlist;
    QStringList m_libraries;
};

// Takes one file out of a playlist in every library on the stick that has
// both, by path: export.pdb's entries rewritten as rekordbox does, the
// OneLibrary rows renumbered, the Engine entry unlinked. A library whose
// playlist does not hold the file is left alone.
class RemoveFromPlaylistChange : public PendingChange
{
public:
    RemoveFromPlaylistChange(QString pioneerRoot, QString enginePath, std::string playlist, std::string filePath,
                             QString title, QStringList libraries);

    static QString idFor(const std::string &playlist, const std::string &filePath);

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
    QString m_pioneerRoot;
    QString m_enginePath;
    std::string m_playlist;
    std::string m_filePath;
    QString m_title;
    QStringList m_libraries;
};

}  // namespace seabass::gui
