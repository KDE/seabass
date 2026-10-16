// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <optional>
#include <string>
#include <vector>

#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Where a playlist at `playlist` (its full path, "Folder/List") is on the
// stick, read now, each library opened once: libraries are "rekordbox",
// "onelibrary", "engine". Slow on a stick (each catalog is read whole), so
// Browse asks it off the GUI thread.
struct PlaylistWhereabouts
{
    // The libraries the stick has at all: where a new playlist goes.
    QStringList onStick;
    // The ones holding a playlist at that path: what an edit of it writes,
    // and backs up.
    QStringList having;
    // The ones where more than one playlist spells that path (two of one
    // name in a folder, or a name holding a "/"): editing by path would
    // have to guess there, so Browse refuses, and the changes fail rather
    // than guess if it happens by Save.
    QStringList several;
};
PlaylistWhereabouts playlistWhereabouts(const QString &pioneerRoot, const QString &enginePath, const std::string &playlist);

// Every playlist one library holds, by full path ("Folder/List"), folders
// left out, read now: catalog is "rekordbox" or "onelibrary" with the
// PIONEER folder, or "engine" with the Engine Library folder. What the
// tracks' memberships cannot say: an empty playlist holds none. Nothing
// when the library is missing or cannot be read.
std::vector<std::string> playlistPathsIn(const std::string &catalog, const std::string &path);

// Creates an empty playlist at the top level, last there, in every
// library on the stick named, each through its own writer (#62's
// reference export for the two rekordbox ones). A library that has a
// playlist of that name by Save is left alone (made since, by the player
// or rekordbox); nothing created anywhere is a skip.
class CreatePlaylistChange : public PendingChange
{
public:
    CreatePlaylistChange(QString pioneerRoot, QString enginePath, std::string playlist, QStringList libraries);

    static QString idFor(const std::string &playlist);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QString playlistEdited() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_pioneerRoot;
    QString m_enginePath;
    std::string m_playlist;
    QStringList m_libraries;
};

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
    QString playlistEdited() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_pioneerRoot;
    QString m_enginePath;
    std::string m_playlist;
    QStringList m_libraries;
};

// Puts one file at the end of a playlist in every library on the stick
// named, by path: an export.pdb entry appended as rekordbox does, a
// OneLibrary row after the last, an Engine entry linked last. A library
// whose playlist already holds the file, or that has no row for the file
// (an Engine-only track has no export.pdb row), is left alone and the log
// says so; nothing added anywhere is a skip. The playlist may be one a
// CreatePlaylistChange staged before it makes in the same save. Given a
// place, each library that took the entry then has it moved there the way
// MoveInPlaylistChange writes a move: one change for a track dropped
// between two rows, not an add and a move.
class AddToPlaylistChange : public PendingChange
{
public:
    // toIndex: the place the entry takes in the playlist after the add,
    // from 0, clamped to the end; none, at the end. Same id either way, so
    // a placed add and a plain one for the same file replace each other.
    AddToPlaylistChange(QString pioneerRoot, QString enginePath, std::string playlist, std::string filePath, QString title,
                        QStringList libraries, std::optional<int> toIndex = std::nullopt);

    static QString idFor(const std::string &playlist, const std::string &filePath);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QString playlistEdited() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

    std::optional<int> toIndex() const { return m_toIndex; }

private:
    QString m_pioneerRoot;
    QString m_enginePath;
    std::string m_playlist;
    std::string m_filePath;
    QString m_title;
    QStringList m_libraries;
    std::optional<int> m_toIndex;
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
    QString playlistEdited() const override;
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

// Moves one file within a playlist to `toIndex` (from 0, its place in
// the playlist after the move, as Browse shows it) in every library on the
// stick named, each the way its own software writes an order: export.pdb's
// entries rewritten from entry_index 1 as rekordbox does, the OneLibrary
// rows numbered from 1 again, the Engine entry relinked in its chain. An
// index past the end of a library's copy puts the file last there. A
// library whose playlist does not hold the file, or holds it more than
// once (which copy moves would be a guess), is left alone and the log says
// why; nothing moved anywhere is a skip.
class MoveInPlaylistChange : public PendingChange
{
public:
    MoveInPlaylistChange(QString pioneerRoot, QString enginePath, std::string playlist, std::string filePath,
                         QString title, int toIndex, QStringList libraries);

    static QString idFor(const std::string &playlist, const std::string &filePath);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QString playlistEdited() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

    int toIndex() const { return m_toIndex; }

private:
    QString m_pioneerRoot;
    QString m_enginePath;
    std::string m_playlist;
    std::string m_filePath;
    QString m_title;
    int m_toIndex = 0;
    QStringList m_libraries;
};

// Renames a playlist or a folder (the last segment of its path; the
// folders above it stay, and a folder's children follow it) in every
// library on the stick named, each through its own writer: export.pdb's
// level rewritten as rekordbox writes a level, OneLibrary's playlist row
// renamed, Engine's title. A library with no playlist at the old path,
// two there, or one at the new path already, is left alone and the log
// says why, and so is an Engine library with smart playlists filed by the
// old path; nothing renamed anywhere is a skip. A name that is empty
// (trimmed), or holds "/" or ";", fails.
class RenamePlaylistChange : public PendingChange
{
public:
    RenamePlaylistChange(QString pioneerRoot, QString enginePath, std::string playlist, std::string newName,
                         QStringList libraries);

    static QString idFor(const std::string &playlist);
    // `playlist` with its last segment replaced by `newName`.
    static std::string renamedPath(const std::string &playlist, const std::string &newName);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    // The playlist by the path it has before the save: one playlist edit.
    QString playlistEdited() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_pioneerRoot;
    QString m_enginePath;
    std::string m_playlist;
    std::string m_newName;
    QStringList m_libraries;
};

}  // namespace seabass::gui
