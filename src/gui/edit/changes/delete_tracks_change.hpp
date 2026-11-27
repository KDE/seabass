// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <memory>
#include <string>
#include <vector>

#include "domain/track.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Deletes tracks that are in no playlist (Library Health, "Tracks not in
// any playlist") the way Clean Up deletes a duplicate: each file's row
// comes out of every library on the stick that lists it, and the file
// goes on the stick's pending-deletions list (Seabass/orphaned), naming
// the backup that still holds the rows, for Free Up Space to
// remove. One change for the whole selection, so each database is read
// once. Every file is checked again first: one that is in a playlist in
// any library by then is left alone and counted as skipped.
//
// Browse stages the same for the tracks of a playlist it deletes ("Delete
// playlist and its tracks", forPlaylist()), behind that playlist's
// DeletePlaylistChange in the same save. The check at Save is what makes
// it safe there: with the playlist gone by its turn, a track goes only if
// no other playlist in any library holds it by then, and if the playlist
// could not be deleted its tracks are still listed and all stay. Browse
// gives each entry a row for every library the stick has, whether or not
// the library listed the file when it was asked: the check at Save then
// looks the file up in each of them as it is by then.
class DeleteTracksChange : public PendingChange
{
public:
    struct Entry
    {
        std::string filePath;
        std::string title;
        std::string artist;
        std::vector<domain::Track> rows;  // every library's row for the file
        // The libraries whose row in `rows` only names the file: they had
        // no row for it when Browse asked (entriesOf). One of these still
        // without a row at Save has nothing to remove; a real row missing
        // by then fails the save.
        std::vector<std::string> standIns;
    };

    DeleteTracksChange(QString pioneerRoot, QString enginePath, std::vector<Entry> entries);
    // The tracks of `playlist` (its full path as Browse shows it), for
    // Browse's editor ("addcue"), under an id of the playlist's own.
    static std::unique_ptr<DeleteTracksChange> forPlaylist(QString pioneerRoot, QString enginePath, std::string playlist,
                                                           std::vector<Entry> entries);

    static QString idFor();
    static QString idForPlaylist(const std::string &playlist);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    int unitsWritten() const override;
    QStringList formatsTouched() const override;
    QStringList targetPaths() const override { return {m_pioneerRoot, m_enginePath}; }
    // A playlist's tracks are written after everything else in the save
    // (see PendingChange::appliesLast). Its own unit, on purpose: tied to
    // the playlist's delete, a cancel would let it land without the
    // changes between the two, and one of those may be the add that puts
    // a track into another playlist to keep it. A cancel stops it
    // instead, and Browse then drops it (PlaylistEditController): the
    // tracks stay on the stick, in no playlist.
    bool appliesLast() const override { return !m_playlist.empty(); }
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    bool has(const std::string &format) const;

    QString m_pioneerRoot;
    QString m_enginePath;
    std::vector<Entry> m_entries;
    // Set for a playlist's tracks (forPlaylist()); empty for Library
    // Health's tracks in no playlist.
    std::string m_playlist;
    int m_deleted = 0;
};

}  // namespace seabass::gui
