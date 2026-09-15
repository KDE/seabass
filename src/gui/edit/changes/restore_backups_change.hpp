// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <vector>

#include "gui/edit/pending_change.hpp"
#include "gui/undo_tracking.hpp"

namespace seabass::gui
{

// The undo of one save ("Undo Last Save"): puts every file that save backed
// up back the way it was, newest backup first.
//
// All or nothing. A save makes one backup record per kind of change, and
// each is only half the undo on its own. If any record is gone -- deleted
// by `seabass-cli --clean`, or released for space -- nothing is restored
// and the error names the missing ones. This used to skip a missing record,
// restore the rest and report the undo as done, leaving the stick half in
// one state and half in the other while the page said all was well. And if
// a restore fails part-way, what was already restored is put back by the
// save's rollback, since every file about to be overwritten is protected
// first.
class RestoreBackupsChange : public PendingChange
{
public:
    explicit RestoreBackupsChange(std::vector<UndoableBackup> backups);

    QString id() const override { return QStringLiteral("undo:last-save"); }
    QString description() const override;
    QString unit() const override { return QStringLiteral("undo steps"); }
    QStringList formatsTouched() const override { return {"rekordbox", "engine", "onelibrary"}; }

    ChangeOutcome apply(SaveContext &ctx) override;

private:
    std::vector<UndoableBackup> m_backups;
};

}  // namespace seabass::gui
