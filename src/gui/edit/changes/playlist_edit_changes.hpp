// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "domain/playlist_tracks_deletion.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::infrastructure::rekordbox
{
class PdbRowWriter;
}

namespace seabass::gui
{

// A playlist's tracks across every library on the stick, with what
// deleting them would come to, when the asker wanted them (Browse's
// "Delete playlist and its tracks"). playlistWhereabouts() never reads
// them: PlaylistEditController does, beside it, in the same turn off the
// GUI thread.
struct PlaylistTracksRead
{
    domain::PlaylistTracksDeletion plan;
    // Libraries the stick has whose tracks could not be read, or why none
    // could: with either, which files another playlist still uses is not
    // known, and nothing may be deleted.
    QStringList unreadable;
    QString error;
};

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
    // The ones where the node at that path is a folder (rekordbox,
    // OneLibrary), counted in `having` all the same. A folder is not the
    // playlist Browse lists by that name: it takes no tracks, and an add,
    // a rename or a delete of the playlist leaves that library alone.
    QStringList folders;
    // The ones where other playlists lie below the node at that path, also
    // counted in `having`. Engine has no folders of its own kind: a
    // playlist holding others is one, and keeps tracks of its own. A
    // delete of it would take the ones inside along, so Browse's delete
    // leaves that library alone; a rekordbox or OneLibrary folder holding
    // playlists is listed here too.
    QStringList holdingOthers;
    // Asked with a file: the libraries on the stick with a row for it,
    // looked up as AddToPlaylistChange looks it up (export.pdb by its path
    // on the stick, OneLibrary's content rows, Engine's tracks by the path
    // relative to the library). A library holding the playlist that is not
    // here would be left alone by an add, so Browse asks first.
    QStringList withFile;
    // Set by the asker that read them (see PlaylistTracksRead); null from
    // playlistWhereabouts() itself.
    std::shared_ptr<const PlaylistTracksRead> tracks;
};
PlaylistWhereabouts playlistWhereabouts(const QString &pioneerRoot, const QString &enginePath, const std::string &playlist,
                                        const std::string &filePath = {});

// Every playlist one library holds, by full path ("Folder/List"), folders
// left out, read now: catalog is "rekordbox" or "onelibrary" with the
// PIONEER folder, or "engine" with the Engine Library folder. What the
// tracks' memberships cannot say: an empty playlist holds none. Nothing
// when the library is missing or cannot be read.
std::vector<std::string> playlistPathsIn(const std::string &catalog, const std::string &path);

class SaveContext;

// Creates an empty playlist at `playlist` ("Folder/List") in each library
// named ("rekordbox", "onelibrary", "engine") that has none at that path
// by now, during a save, each through its own writer (#62's reference
// export for the two rekordbox ones), last in its level. A path with a
// folder goes into that folder, which export.pdb and OneLibrary must
// already have (Engine makes it). Returns the libraries it was created in;
// throws when one cannot be written or holds the path more than once.
// CreatePlaylistChange and AlignPlaylistChange (a playlist only some
// libraries have, followed from one that has it) both create through it.
// export.pdb's rows go into the save's shared buffer (sharedPdbRows()): a
// caller that opens the file itself afterwards lands them first
// (SaveContext::landHeldWrites()).
QStringList createPlaylistIn(SaveContext &ctx, const QString &pioneerRoot, const QString &enginePath,
                             const std::string &playlist, const QStringList &libraries, int itemCountHint,
                             const char *label);

// Creates an empty playlist at the top level, last there, in every
// library on the stick named, each through its own writer (#62's
// reference export for the two rekordbox ones). A library that has a
// playlist of that name by Save is left alone (made since, by the player
// or rekordbox); nothing created anywhere is a skip.
class CreatePlaylistChange : public PendingChange
{
public:
    // itemCountHint: how many changes the save this is staged in writes
    // to each library, when it is the first of them: the first change to
    // write a library decides whether the save works on a scratch copy of
    // it, and a lone playlist's 1 would decide "no" for a batch of two
    // hundred imports staged behind it.
    CreatePlaylistChange(QString pioneerRoot, QString enginePath, std::string playlist, QStringList libraries,
                         int itemCountHint = 1);

    // Makes this playlist one unit with the changes that carry `key`: a
    // playlist made for the tracks imported into it is not left behind
    // empty by a cancel before the first of them.
    void tieTo(const QString &key) { m_unitKey = key; }
    QStringList unitKeys() const override;
    // Staged by the track import: an id of its own ("addcue:import:"), so
    // that the playlist editor's change for the same playlist, staged
    // after it, is staged beside it and not over it (which would undo the
    // tie above, and take the editor's change away with the import's).
    // Both may stand in one save: a playlist that is there by its turn is
    // not made again.
    void stageForImport() { m_forImport = true; }

    static QString idFor(const std::string &playlist);
    // The id the track import's create of `playlist` has (stageForImport).
    static QString importIdFor(const std::string &playlist);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QString playlistEdited() const override;
    QStringList formatsTouched() const override;
    QStringList targetPaths() const override { return {m_pioneerRoot, m_enginePath}; }
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;
    // export.pdb only through sharedPdbRows().
    bool sharesHeldWrites() const override { return true; }

private:
    QString m_pioneerRoot;
    QString m_enginePath;
    std::string m_playlist;
    QStringList m_libraries;
    int m_itemCountHint = 1;
    QString m_unitKey;
    bool m_forImport = false;
};

// Deletes a playlist from every library on the stick named, each through
// its own writer, the way rekordbox does it (#62). Tracks stay. A library
// where the node at that path is a folder (rekordbox, OneLibrary), or one
// holding other playlists (an Engine playlist with others inside), is left
// alone: a delete there would take everything below it along. The log and
// the save's summary say so; nothing deleted anywhere is a skip. Staged from Browse, whose editor
// is "addcue", and from Library Health's Playlists check, whose session
// belongs to "library-health": a session refuses a change of another
// owner than the batch's, so the stager says whose it is. A page that
// stages it among many changes of its own (Sync after Rekordbox Export)
// gives the save's item count and its own backup label: the first change
// of a save to write a library decides how its write session works and
// under which label its backups are filed.
class DeletePlaylistChange : public PendingChange
{
public:
    DeletePlaylistChange(QString pioneerRoot, QString enginePath, std::string playlist, QStringList libraries,
                         QString owner = QStringLiteral("addcue"), int itemCountHint = 1,
                         std::string label = "playlist-delete");

    static QString idFor(const std::string &playlist);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QString playlistEdited() const override;
    QStringList formatsTouched() const override;
    QStringList targetPaths() const override { return {m_pioneerRoot, m_enginePath}; }
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;
    // export.pdb only through sharedPdbRows().
    bool sharesHeldWrites() const override { return true; }

private:
    QString m_pioneerRoot;
    QString m_enginePath;
    std::string m_playlist;
    QStringList m_libraries;
    QString m_owner;
    int m_itemCountHint = 1;
    std::string m_label;
};

// Puts one file at the end of a playlist in every library on the stick
// named, by path: an export.pdb entry appended as rekordbox does, a
// OneLibrary row after the last, an Engine entry linked last. A library
// whose playlist already holds the file, or that has no row for the file
// (an Engine-only track has no export.pdb row), is left alone and the log
// says so, as is an export.pdb or OneLibrary whose node at that path is a
// folder (an entry against it is a row no player reads; the save's summary
// says that one too); nothing added anywhere is a skip. The playlist may be one a
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
    QStringList targetPaths() const override { return {m_pioneerRoot, m_enginePath}; }
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;
    // export.pdb only through sharedPdbRows().
    bool sharesHeldWrites() const override { return true; }

    std::optional<int> toIndex() const { return m_toIndex; }
    const std::string &playlist() const { return m_playlist; }
    const std::string &filePath() const { return m_filePath; }
    bool stagedForImport() const { return m_forImport; }

    // Makes this entry one unit with the changes that carry `key` (see
    // PendingChange::unitKeys): a track imported in the same save and
    // put into its playlist is not left imported and unlisted by a
    // cancel between the two.
    void tieTo(const QString &key) { m_unitKey = key; }
    QStringList unitKeys() const override;
    // Staged by the track import: an id of its own, as
    // CreatePlaylistChange::stageForImport() says. Both may stand in one
    // save: a file the playlist holds by its turn is not added again.
    void stageForImport() { m_forImport = true; }

private:
    QString m_pioneerRoot;
    QString m_enginePath;
    std::string m_playlist;
    std::string m_filePath;
    QString m_title;
    QStringList m_libraries;
    std::optional<int> m_toIndex;
    QString m_unitKey;
    bool m_forImport = false;
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
    QStringList targetPaths() const override { return {m_pioneerRoot, m_enginePath}; }
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;
    // export.pdb only through sharedPdbRows().
    bool sharesHeldWrites() const override { return true; }

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
    QStringList targetPaths() const override { return {m_pioneerRoot, m_enginePath}; }
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;
    // export.pdb only through sharedPdbRows().
    bool sharesHeldWrites() const override { return true; }

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

// The one playlist export.pdb has at `path` ("Folder/List"), which is not
// a folder; nothing when it has none, or a folder, there. Throws when
// several rows spell the path. For a caller rehearsing playlist entries
// on a writer of its own (ImportRehearsal).
std::optional<std::uint32_t> pdbPlaylistAt(const infrastructure::rekordbox::PdbRowWriter &rows, const std::string &path);

// Moves a run of files already in a playlist to one place, in the order
// given, in every library on the stick named: what a batch of imported
// tracks dropped between two rows needs after each was added at the end.
// One rewrite of the playlist per library, however many files: placing
// each as it is added (AddToPlaylistChange's toIndex) rewrites
// export.pdb's entries of the whole playlist once per file. A library
// whose playlist does not hold every one of the files exactly once is
// left alone and the log says so; nothing moved anywhere is a skip.
class PlaceInPlaylistChange : public PendingChange
{
public:
    // toIndex: where the first file goes once the run is taken out of
    // the playlist, from 0, clamped to the end.
    PlaceInPlaylistChange(QString pioneerRoot, QString enginePath, std::string playlist, std::vector<std::string> files,
                          int toIndex, QStringList libraries);

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
    // export.pdb only through sharedPdbRows().
    bool sharesHeldWrites() const override { return true; }

    int toIndex() const { return m_toIndex; }
    const std::vector<std::string> &files() const { return m_files; }
    // `file` is no longer of the run (the track import's "Don't import"
    // for one track): the rest are moved without it. Not while a save
    // writes. False, nothing changed, when it is the last of the run,
    // which is then taken back whole instead.
    bool leaveOut(const std::string &file);

private:
    QString m_pioneerRoot;
    QString m_enginePath;
    std::string m_playlist;
    std::vector<std::string> m_files;
    int m_toIndex = 0;
    QStringList m_libraries;
};

// `order` with the ids of `run` taken out and put back together at
// `toIndex` (counted in what is left, clamped to its end), in `run`'s
// order. Nothing when `order` does not hold each of them exactly once, or
// `run` names one twice.
template<typename Id>
std::optional<std::vector<Id>> orderWithRunAt(const std::vector<Id> &order, const std::vector<Id> &run, int toIndex)
{
    for (size_t i = 0; i < run.size(); ++i) {
        if (std::count(order.begin(), order.end(), run[i]) != 1 || std::count(run.begin(), run.end(), run[i]) != 1) {
            return std::nullopt;
        }
    }
    std::vector<Id> rest;
    for (const Id &id : order) {
        if (std::find(run.begin(), run.end(), id) == run.end()) {
            rest.push_back(id);
        }
    }
    const size_t to = std::min(static_cast<size_t>(std::max(toIndex, 0)), rest.size());
    rest.insert(rest.begin() + static_cast<std::ptrdiff_t>(to), run.begin(), run.end());
    return rest;
}

// Renames a playlist (the last segment of its path; the folders above it
// stay, and an Engine playlist's children follow it) in every library on
// the stick named, each through its own writer: export.pdb's level
// rewritten as rekordbox writes a level, OneLibrary's playlist row
// renamed, Engine's title. A library with no playlist at the old path,
// two there, or one at the new path already, is left alone and the log
// says why, and so is an Engine library with smart playlists filed by the
// old path, and a rekordbox or OneLibrary library where the node at the
// old path is a folder (the save's summary says that one too); nothing
// renamed anywhere is a skip. A name that is empty
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
    QStringList targetPaths() const override { return {m_pioneerRoot, m_enginePath}; }
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;
    // export.pdb only through sharedPdbRows().
    bool sharesHeldWrites() const override { return true; }

private:
    QString m_pioneerRoot;
    QString m_enginePath;
    std::string m_playlist;
    std::string m_newName;
    QStringList m_libraries;
};

}  // namespace seabass::gui
