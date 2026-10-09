// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <QString>
#include <QStringList>

#include <string>
#include <utility>
#include <vector>

#include "domain/rekordbox_baseline.hpp"
#include "gui/edit/pending_change.hpp"

namespace seabass::gui
{

// Sync after Rekordbox Export's record of the stick as its save leaves it:
// the rekordbox baseline (<stick>/Seabass/rekordbox-baseline.tsv.gz), the
// B of the next three-way plan (docs/sync-after-rekordbox-export-plan.md,
// "When the baseline is written", case 1). Staged last by the page.
//
// `next` is what the page would record if everything it staged lands
// (domain::nextBaseline). `appliedBy` names, for each staged change by its
// id as the save reports it (the wrapped id,
// "rekordbox-export-sync:sync:engine:6"), the item keys that change
// carries. The record is made after the commit, from the stick as the save
// left it:
//
//   - the items of every change that did not land keep the previous
//     baseline's value (domain::keepPreviousItems), so a save that stopped
//     at a failure records only what landed;
//   - every rekordbox write of the changes that landed is merged as
//     Seabass's own (the origin ledger, domain::recordSeabassWrites);
//   - the pdb sequence is export.pdb's as the save left it, re-read, since
//     a restore onto rekordbox may have rewritten it;
//   - written atomically; nothing at all when no change landed.
//
// Registered in beforeSave(), not apply(): a save stopped by a failure or
// a cancel never reaches the last change, and what did land still has to
// be recorded. Then this change stays pending with the ones that did not
// land, and the retry records again on top of what this one wrote: the
// items of changes applied the first time keep the value recorded then.
//
// The baseline file is declared as a backup target, as absent when the
// stick has none yet (BackupTarget::removeOnRestoreIfAbsent), so Undo Last
// Save puts the previous one back, or takes away the first.
//
// Counted as no unit: the summary sums every change's units under the
// first change's noun ("12 of 12 tracks added"), and a record of the save
// is not one of the things the user asked for. Unit "records", verb
// "recorded", for a save that holds nothing else.
class RecordRekordboxBaselineChange : public PendingChange
{
public:
    using AppliedBy = std::vector<std::pair<std::string, std::vector<std::string>>>;

    // stickRoot: the stick's root (UTF-8), where Seabass/ lives. The
    // export.pdb whose sequence is recorded is the save's rekordbox path.
    RecordRekordboxBaselineChange(std::string stickRoot, domain::RekordboxBaseline next, AppliedBy appliedBy);

    static QString idString();

    QString id() const override;
    QString description() const override;
    QString unit() const override;
    int unitsWritten() const override { return 0; }
    QString verb() const override;
    QStringList formatsTouched() const override { return {}; }
    std::vector<BackupTarget> filesToBackup(SaveContext &ctx) const override;
    void beforeSave(SaveContext &ctx) override;
    ChangeOutcome apply(SaveContext &ctx) override;

private:
    std::string m_stickRoot;
    domain::RekordboxBaseline m_next;
    AppliedBy m_appliedBy;
};

}  // namespace seabass::gui
