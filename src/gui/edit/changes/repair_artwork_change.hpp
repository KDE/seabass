// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include "gui/edit/pending_change.hpp"
#include "infrastructure/engine/engine_artwork.hpp"

namespace seabass::gui
{

// Gives one Engine track its cover art the way Engine stores it: the image
// copied into "Engine Library/Artwork" under the hash of its bytes, and the
// track pointed at that row.
//
// The art these tracks have now is a path from the computer that ran
// Engine's "import rekordbox library" ("image://fileart//media/<label>/
// PIONEER/Artwork/..."), which no player can follow. The images themselves
// are on the stick, so this is a copy plus a row, never a download or a
// re-analysis.
//
// One track per change, like every other change on this page: the unit the
// save summary counts is the track, the progress bar ticks once per track,
// and Cancel is looked at between them. Staged as a batch it reported "1 of
// 1 tracks repaired" for a stick with 1174 of them, gave the whole copy one
// tick, and could not be cancelled at all. The database is written through
// the save's shared FormatWriteSession, so the thousand writes still go to
// one scratch copy and come back to the stick in one atomic replace.
class RepairArtworkChange : public PendingChange
{
public:
    // itemCountHint: how many of these the save may carry, so the first one
    // can decide whether the whole run goes through a scratch copy.
    // declaresDatabase: true for the first of a batch only, see
    // filesToBackup().
    RepairArtworkChange(QString enginePath, infrastructure::engine::ArtworkEntry entry, int itemCountHint,
                        bool declaresDatabase = true);

    QString id() const override;
    // Staged from Library Health, like the repairs and orphan deletions.
    QString owner() const override;
    QString description() const override;
    QString unit() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    ChangeOutcome apply(SaveContext &ctx) override;

    // The id this change will have, so the page can unstage it without
    // holding the change itself.
    static QString idFor(std::int64_t trackId);

private:
    QString m_enginePath;
    infrastructure::engine::ArtworkEntry m_entry;
    int m_itemCountHint = 1;
    bool m_declaresDatabase = true;
};

}  // namespace seabass::gui
