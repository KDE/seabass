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

// Sync after Rekordbox Export: one Engine playlist's entries brought in
// line with rekordbox's copy of it, one change per playlist (the
// proposal's MembershipEdits for that playlist), removes first, then adds
// in rekordbox's order.
//
// Tracks are found by their file at apply time
// (infrastructure::engine::engineTrackIdsForFile, in the save's write
// root), never by an id from the scan, so an add staged after an
// AddEngineTrackChange of the same save finds the row that change made.
//
// An add goes right after its anchor, the member before it in rekordbox's
// playlist that Engine holds (insertIntoEnginePlaylist), or first when it
// has none (insertAtStartOfEnginePlaylist; MembershipEdit's empty
// afterPathKey). It is appended when the anchor is missing at apply time:
// the anchor's file has no Engine row (its add was left unticked) or that
// row is not in the playlist. A track already in the playlist stays where
// it is and counts as already there. A remove takes the track's entries
// out (removeFromEnginePlaylist); the row and its other playlists stay.
// Each insert and remove reads its list back as one chain in the order
// asked before it commits.
//
// Fails the change, the save loop putting m.db back, when: the playlist
// path does not name exactly one Engine playlist (none: its create was
// not staged before this; several: ambiguous), an added file or an anchor
// file has two Engine rows (Clean Up first), an added file has none, or a
// remove's id is not one of its file's rows. The two-row refusal cannot
// fire on a schema with UNIQUE(Track.path), 3.x's, since rows are found by
// the exact path; it stays for one without, and two spellings of one file
// are the planner's DuplicateEngineRows, which stages nothing. A remove whose file has no
// row any more is left out (gone since the scan). Nothing to do at all is
// a skip.
//
// Unit "playlists", verb "updated"; the description counts the entries.
// Writes the save's one Engine write session, m.db declared, Engine 1.x
// refused.
class EngineMembershipChange : public PendingChange
{
public:
    struct Add
    {
        // Absolute, on the stick as it is mounted now.
        std::string filePath;
        // The member to put it after, by its file; nullopt: first in the
        // playlist.
        std::optional<std::string> afterFilePath;
        std::string title;  // for the log
    };
    struct Remove
    {
        std::string engineSourceId;
        // What the id was read with, to confirm it by at apply time.
        std::string filePath;
        std::string title;  // for the log
    };

    EngineMembershipChange(QString enginePath, std::string playlistPath, std::vector<Add> adds,
                           std::vector<Remove> removes, int itemCountHint);

    static QString idFor(const std::string &playlistPath);

    QString id() const override;
    QString owner() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

    // What the last apply() did, entry by entry.
    int entriesAdded() const { return m_added; }
    int entriesRemoved() const { return m_removed; }
    // Adds already in the playlist and removes already out of it (or of
    // the library): nothing to write for them.
    int entriesAlreadyDone() const { return m_alreadyDone; }

private:
    QString m_enginePath;
    std::string m_playlist;
    std::vector<Add> m_adds;
    std::vector<Remove> m_removes;
    int m_itemCountHint = 1;
    int m_added = 0;
    int m_removed = 0;
    int m_alreadyDone = 0;
};

}  // namespace seabass::gui
