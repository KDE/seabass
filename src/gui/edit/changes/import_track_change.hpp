// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>
#include <memory>
#include <string>

#include "domain/imported_track.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// The track import: one audio file from the computer put onto the stick
// and given a row in every library the stick has. In order, inside one
// save step:
//   1. the file is copied to its place under the stick's root and
//      verified (infrastructure::track_import::copyFileVerified: never
//      over a file that is there, never from a file that changed while
//      it was read);
//   2. DeviceLibrary: a track row in export.pdb with the lookup rows it
//      needs (PdbRowWriter::addTrack);
//   3. OneLibrary: a content row in exportLibrary.db
//      (OneLibraryCueWriter::addTrack);
//   4. Engine: a Track row in m.db (createEngineTrack), left for the
//      player to analyse.
// All three rows are filled from the one domain::ImportedTrack, so the
// libraries say the same about the track.
//
// `libraries` has to name exactly the libraries the stick has: apply()
// refuses otherwise, before it writes (an import that reaches only some
// of a stick's libraries leaves them disagreeing, which is the state the
// app exists to prevent).
//
// A failed change takes the whole save back (runSaveLoop), every track
// imported before it included. So everything that can be asked before
// the copy is: what each library's writer would refuse the row for, the
// libraries on the stick, the source file. The copy itself is not
// cancellable for the same reason; a cancel waits for the file in hand.
// What is left to fail after the copy is the stick itself.
//
// The copied audio file is declared as a file this change creates, so a
// failed save and Undo Last Save both take it away with the rows. The
// folders the copy made stay, empty.
//
// Not this change's: playlists (AddToPlaylistChange and
// EngineMembershipChange find the new rows by their file, staged after
// this one and tied to it by unitKeys()), covers and analysis files.
// A rehearsal of a batch of imports against the stick's export.pdb as it
// is now: each track's row is added to a copy of the file in memory that
// is never written, so what only the library can refuse a row for, and
// only once the rows before it are in (the last page filling up where no
// new page can be started, a lookup table that cannot take another
// name), is known when the batch is staged. A failed change takes the
// whole save back, so the one track that cannot go in must not be staged
// with the forty that can.
//
// Reads the file once, at construction; writes nothing, ever. A stick
// without export.pdb rehearses nothing and refuses nothing. The other
// two libraries have no state of this kind: what they refuse a row for
// beyond its own contents is the library as a whole (a schema, a missing
// row), which the first change of a save meets with nothing yet to lose.
class ImportRehearsal
{
public:
    // pioneerRoot: empty, or a folder without rekordbox/export.pdb, for a
    // stick without DeviceLibrary. Throws when the file is there and
    // cannot be read as one.
    explicit ImportRehearsal(const QString &pioneerRoot);
    ~ImportRehearsal();
    ImportRehearsal(const ImportRehearsal &) = delete;
    ImportRehearsal &operator=(const ImportRehearsal &) = delete;

    // The playlist the batch goes into, by its path: from here on add()
    // also rehearses the track's entry in it, as the AddToPlaylistChange
    // staged behind each import writes one. With `create`, a playlist
    // export.pdb does not have is made in the copy first, at the top
    // level, as CreatePlaylistChange makes it. Empty when the copy has
    // the playlist now; otherwise why not.
    std::string usePlaylist(const std::string &path, bool create);

    // Adds `track`'s row to the rehearsal copy, and its playlist entry
    // when a playlist is in use. Empty when they went in; otherwise why
    // not. A row whose entry could not follow it stays in the copy: the
    // table that refused it refuses every later one as well.
    std::string add(const domain::ImportedTrack &track, std::uint64_t fileSizeBytes);

    // Moves the entries add() made to `toIndex` in the playlist, in their
    // order, as PlaceInPlaylistChange does once for the batch. Empty when
    // the copy took it (or there is nothing to move); otherwise why not.
    std::string place(int toIndex);

private:
    struct Rows;
    std::unique_ptr<Rows> m_rows;
};

class ImportTrackChange : public PendingChange
{
public:
    // `sourceFile`: the file on the computer, as the bytes of its path (a
    // name that is not UTF-8 does not survive a QString). `stickRoot`: the stick as it
    // is mounted now; `track.stickPath` is the file's place under it.
    // `pioneerRoot` and `enginePath` as every change takes them, empty for
    // a library the stick does not have. `libraries`: "rekordbox",
    // "onelibrary", "engine".
    ImportTrackChange(std::string sourceFile, QString stickRoot, QString pioneerRoot, QString enginePath,
                      domain::ImportedTrack track, QStringList libraries, int itemCountHint);

    // Why the libraries named would refuse a row for `track`, a file of
    // `fileSizeBytes`, for what it is; empty when none would. What
    // apply() asks before it copies, for a caller that plans: a file
    // refused here is left out of the save rather than failing it.
    static std::string rowRefusal(const domain::ImportedTrack &track, const QStringList &libraries,
                                  std::uint64_t fileSizeBytes);

    static QString idFor(const std::string &stickPath);
    // What ties the playlist changes of this track to it.
    static QString unitKeyFor(const std::string &stickPath);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList unitKeys() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

    // The file on the stick: where it is, or will be.
    std::string destinationFile() const;
    // After an apply() that succeeded: the ids of the new rows, 0 or -1
    // for a library that was not written. They name rows only while the
    // save that made them stands.
    std::uint32_t deviceLibraryId() const { return m_deviceLibraryId; }
    std::int64_t oneLibraryId() const { return m_oneLibraryId; }
    std::int64_t engineId() const { return m_engineId; }

private:
    std::string m_sourceFile;
    QString m_stickRoot;
    QString m_pioneerRoot;
    QString m_enginePath;
    domain::ImportedTrack m_track;
    QStringList m_libraries;
    int m_itemCountHint = 1;
    std::uint32_t m_deviceLibraryId = 0;
    std::int64_t m_oneLibraryId = -1;
    std::int64_t m_engineId = -1;
};

}  // namespace seabass::gui
