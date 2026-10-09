// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// The editor Sync after Rekordbox Export stages under. Every change the
// page stages owns this, its own and the ones it borrows through
// OwnedChange. Also the backup label its own changes use, so one save
// is one record in Manage Backups for them.
QString rekordboxExportSyncOwner();

// Another page's change, staged by this one. LibraryEditSession refuses a
// change whose owner() is not the editor of what is already staged (one
// library, one page at a time; see PendingChange::owner), and the changes
// a page reuses keep their own: SyncPlanChange is "sync",
// DeletePlaylistChange "addcue", RestoreMetadataChange "restore-metadata".
// Wrapped, a change answers owner() with the wrapping page and id() with
// that page's prefix ("rekordbox-export-sync:sync:engine:1234"), so two
// pages staging the same change never replace each other's by id, and
// owner() is the id's prefix as the default rule has it.
//
// Everything else is the wrapped change's: the description, the subject,
// the unit and the counts, the verb, the formats, the backup targets, the
// rekordbox writes and the write itself. A virtual added to PendingChange
// has to be forwarded here too, or a wrapped change silently answers with
// the default (owned_change_test checks each one against its inner
// change).
//
// Nothing dynamic_casts a staged change today; a caller that needs the
// wrapped change's own type asks inner().
class OwnedChange : public PendingChange
{
public:
    // Throws std::invalid_argument for an empty owner or no change: a
    // change with no owner would join any page's batch.
    OwnedChange(QString owner, std::unique_ptr<PendingChange> inner);

    const PendingChange &inner() const { return *m_inner; }

    QString id() const override;
    QString description() const override;
    QString subject() const override;
    QString unit() const override;
    int unitsWritten() const override;
    int unitsSkipped() const override;
    QString verb() const override;
    QStringList formatsTouched() const override;
    QString owner() const override;
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    std::vector<RekordboxWrite> rekordboxWrites() const override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    QString m_owner;
    std::unique_ptr<PendingChange> m_inner;
};

// `change`, owned by Sync after Rekordbox Export.
std::unique_ptr<PendingChange> ownedByRekordboxExportSync(std::unique_ptr<PendingChange> change);

}  // namespace seabass::gui
