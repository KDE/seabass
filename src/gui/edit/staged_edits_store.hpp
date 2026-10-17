// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>

#include <optional>
#include <vector>

namespace seabass::gui
{

// One playlist edit as Browse asked for it, at PlaylistEditController's
// own level: the call that staged it, not the PendingChange it made.
// Kept so the call can be made again (see StagedEditsStore).
//
//   kind: "create", "delete", "add", "remove", "move" or "rename"
//   playlist: the playlist as the page named it; for a rename, the path
//             the stick has
//   file: the track's file, absolute in memory, relative to the stick's
//         mount point on disk
//   anyway: an add the user confirmed past "Not in every library"
struct StagedEditRequest
{
    QString kind;
    QString playlist;
    QString file;
    QString title;
    int toIndex = -1;
    QString newName;
    bool anyway = false;

    bool operator==(const StagedEditRequest &) const = default;
};

struct StagedEdits
{
    QString libraryId;
    QString stickLabel;
    QString savedAtUtc;
    QString appVersion;
    // In the order they were staged.
    std::vector<StagedEditRequest> requests;
};

// Browse's staged playlist edits, kept on this machine while they are
// staged, so a crash does not take them along. Seabass crashed a minute
// after a save on 2026-10-17, and staged edits lived only in memory
// until Save: a batch someone spent half an hour on would have gone with
// it. Now the controller writes the whole batch here every time it
// changes, and Browse offers it back when the stick is opened again.
//
// A snapshot, not a log: one JSON file per library id, rewritten whole,
// removed when nothing is staged. Local, never on the stick: staging
// never writes the stick, which may be a browsed backup, read-only, or
// pulled. Next to the edit-lock cookies (appDataDirectory()), named
// after the sanitised library id.
//
// File paths are kept relative to the stick's mount point and re-rooted
// on the mount point of the day when read: the same stick mounts
// somewhere else tomorrow.
class StagedEditsStore
{
public:
    // appDataDirectory()/"staged-edits".
    StagedEditsStore();
    // A test's own directory.
    explicit StagedEditsStore(QString directory);

    QString directory() const { return m_directory; }
    QString pathFor(const QString &libraryId) const;

    // Writes `edits` whole (atomically), its files made relative to
    // `mountPoint`; with no requests, removes the file instead. False when
    // the file could not be written.
    bool write(const StagedEdits &edits, const QString &mountPoint) const;
    // The edits kept for `libraryId`, their files re-rooted on
    // `mountPoint`; nothing when there is no file, it holds no edit, or
    // it cannot be read.
    std::optional<StagedEdits> read(const QString &libraryId, const QString &mountPoint) const;
    void remove(const QString &libraryId) const;

    // `path` relative to `mountPoint` with '/' separators, or `path` as it
    // is when it does not lie below it.
    static QString relativeTo(const QString &mountPoint, const QString &path);
    // `relative` under `mountPoint`, spelled with the platform's
    // separators as the scan spells a track's file; an absolute path is
    // left as it is.
    static QString reroot(const QString &mountPoint, const QString &relative);

private:
    QString m_directory;
};

}  // namespace seabass::gui
